/*
 * scc2m2v — SCC -> libcaption -> AV_FRAME_DATA_A53_CC -> FFmpeg mpeg2video
 *
 *   ffmpeg -i src ... -f rawvideo -pix_fmt yuv420p - | scc2m2v caps.scc out.m2v [opts]
 *
 * Reads raw yuv420p frames on stdin (already 1920x1080 @ 30000/1001), attaches a
 * per-frame A/53 cc_data triplet list as side data, encodes MPEG-2, and writes the
 * raw video elementary stream. FFmpeg's MPEG-2 encoder writes the
 * "GA94 / 03 / flags+cc_count / FF / triplets / FF" user-data wrapper itself.
 *
 * Per frame the triplets are:
 *   [0]      FC hi lo   608 field 1 (real data from the SCC, or 80 80 null pad)
 *   [1]      F9 00 00   608 field 2 (invalid / unused)
 *   [2..N-1] FF/FE ..   CEA-708 DTVCC packet bytes (service 1), else FA 00 00 filler
 */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>

#include "caption.h"
#include "cea708.h"
#include "mpeg.h"
#include "scc.h"
#include "utf8.h"

/* FFmpeg 7 renamed FF_PROFILE_* to AV_PROFILE_*; accept either */
#ifdef AV_PROFILE_MPEG2_HIGH
#define MPEG2_PROFILE_HIGH AV_PROFILE_MPEG2_HIGH
#else
#define MPEG2_PROFILE_HIGH FF_PROFILE_MPEG2_HIGH
#endif

/* ------------------------------------------------------------------------- */
/* 1. SCC -> per-frame 608 pair schedule, using libcaption's SEI path          */
/* ------------------------------------------------------------------------- */

static uint16_t* sched = NULL; /* sched[frame] = 608 pair, 0 = none */
static size_t sched_len = 0;

static void sched_put(size_t f, uint16_t v)
{
    if (f >= sched_len) {
        size_t n = (f + 1) * 2 + 1024;
        sched = realloc(sched, n * sizeof(uint16_t));
        memset(sched + sched_len, 0, (n - sched_len) * sizeof(uint16_t));
        sched_len = n;
    }
    sched[f] = v;
}

/* 29.97 timecode -> frame index. ';' = drop-frame, ':' = non-drop */
static long tc_to_frame(int hh, int mm, int ss, int ff, int drop)
{
    long total_min = hh * 60 + mm;
    long f = ((total_min * 60L) + ss) * 30L + ff;
    if (drop)
        f -= 2 * (total_min - total_min / 10);
    return f;
}

/*
 * Pull the CC triplets out of a libcaption T.35 message.
 * libcaption payload (H.264 T.35 form, NO emulation prevention yet):
 *   [0]    B5            country code
 *   [1..2] 00 31         provider (ATSC)
 *   [3..6] 'GA94'
 *   [7]    03            user_data_type_code
 *   [8]    flags|cc_count
 *   [9]    FF            em_data
 *   [10..] cc_count * 3  <-- what FFmpeg wants
 *   [last] FF            marker
 */
static int extract_triplets(const uint8_t* d, size_t size, const uint8_t** trip)
{
    if (size < 11 || d[0] != 0xB5 || d[1] != 0x00 || d[2] != 0x31 || memcmp(d + 3, "GA94", 4) || d[7] != 0x03)
        return -1;
    int cnt = d[8] & 0x1F;
    if (size < (size_t)(10 + cnt * 3))
        return -1;
    *trip = d + 10;
    return cnt;
}

static long tc_start_frame = 0; /* SCC timecode of the first video frame */

static long load_scc(const char* path)
{
    size_t sz = 0;
    utf8_char_t* txt = utf8_load_text_file(path, &sz);
    if (!txt) {
        fprintf(stderr, "cannot read %s\n", path);
        return -1;
    }
    scc_t* scc = NULL;
    long cursor = 0, slipped = 0, lines = 0, pairs = 0;
    char* save = NULL;
    char* copy = strdup(txt);
    for (char* line = strtok_r(copy, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        int hh, mm, ss, ff;
        char sep;
        if (5 != sscanf(line, "%d:%d:%d%c%d", &hh, &mm, &ss, &sep, &ff))
            continue; /* header / blank / garbage */
        long start = tc_to_frame(hh, mm, ss, ff, sep == ';') - tc_start_frame;
        if (start < 0)
            continue; /* before the first video frame */

        /* NOTE: libcaption's scc_to_608() is bypassed: utf8_line_length() double-advances
         * its pointer, so it under-measures the line and silently drops ~half the words. */
        const char* w = line;
        while (*w && *w != ' ' && *w != '\t') w++;          /* skip timecode */
        int nw = 0;
        for (const char* q2 = w; *q2; q2++) if (*q2 == ' ' || *q2 == '\t') nw++;
        free(scc);
        scc = scc_new(nw + 1);
        unsigned int hex, used;
        while (*w && 1 == sscanf(w, " %4x%n", &hex, &used)) {
            scc->cc_data[scc->cc_size++] = (uint16_t)hex;
            w += used;
        }
        if (!scc->cc_size)
            continue;

        /* libcaption: SCC words -> SEI T.35 messages (<=31 triplets each) */
        sei_t sei;
        sei_init(&sei, 0);
        sei_from_scc(&sei, scc);
        long slot = start > cursor ? start : cursor;
        if (slot > start)
            slipped++;
        for (sei_message_t* m = sei_message_head(&sei); m; m = sei_message_next(m)) {
            if (sei_message_type(m) != sei_type_user_data_registered_itu_t_t35)
                continue;
            const uint8_t* t;
            int n = extract_triplets(sei_message_data(m), sei_message_size(m), &t);
            for (int i = 0; i < n; i++, t += 3) {
                int valid = (t[0] >> 2) & 1, type = t[0] & 3;
                if (valid && type == 0) { /* field-1 608 pair */
                    sched_put(slot++, (uint16_t)((t[1] << 8) | t[2]));
                    pairs++;
                }
            }
        }
        cursor = slot;
        sei_free(&sei);
        lines++;
    }
    fprintf(stderr, "SCC: %ld lines, %ld 608 pairs, %ld lines delayed by backlog, last pair at frame %ld\n", lines, pairs, slipped, cursor);
    free(copy);
    free(txt);
    free(scc);
    return cursor;
}

/* ------------------------------------------------------------------------- */
/* 2. Minimal CEA-708 (service 1) generator, driven by libcaption's 608 decode */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint8_t type, a, b;
} trip_t; /* type: 2 = DTVCC data, 3 = DTVCC start */

static trip_t* q = NULL;
static size_t q_head = 0, q_len = 0, q_cap = 0;
static int pkt_seq = 0;

static void q_push(uint8_t type, uint8_t a, uint8_t b)
{
    if (q_len == q_cap) {
        if (q_head) {
            memmove(q, q + q_head, (q_len - q_head) * sizeof(trip_t));
            q_len -= q_head;
            q_head = 0;
        }
        if (q_len == q_cap) {
            q_cap = q_cap * 2 + 256;
            q = realloc(q, q_cap * sizeof(trip_t));
        }
    }
    q[q_len++] = (trip_t) { type, a, b };
}

static void emit_packet(const uint8_t* blocks, int len)
{
    uint8_t buf[130];
    int n = 0;
    int data = len;
    if (!(data & 1)) /* packet_data_size must be odd -> pad with a null block */
        data++;
    int code = (1 + data) / 2;
    buf[n++] = (uint8_t)(((pkt_seq++ & 3) << 6) | (code & 0x3F));
    memcpy(buf + n, blocks, len), n += len;
    if (data > len)
        buf[n++] = 0x00;
    for (int i = 0; i < n; i += 2)
        q_push(i == 0 ? 3 : 2, buf[i], buf[i + 1]);
}

/* atomic-unit packer: units -> service blocks (<=31B) -> one packet per block */
static uint8_t pk[128];
static int pk_len;
static uint8_t blk[32];
static int blk_len;

static void blk_flush(void)
{
    if (!blk_len)
        return;
    /* One service block per DTVCC packet: legal, and VLC's 708 decoder only honours the
     * first service block of each packet. */
    pk_len = 0;
    pk[pk_len++] = (uint8_t)((1 << 5) | blk_len); /* service 1 */
    memcpy(pk + pk_len, blk, blk_len);
    pk_len += blk_len;
    blk_len = 0;
    emit_packet(pk, pk_len);
    pk_len = 0;
}
static void unit(const uint8_t* u, int n)
{
    if (blk_len + n > 31)
        blk_flush();
    memcpy(blk + blk_len, u, n);
    blk_len += n;
}
static void pack_begin(void) { pk_len = blk_len = 0; }
static void pack_end(void)
{
    blk_flush();
    if (pk_len)
        emit_packet(pk, pk_len);
    pk_len = 0;
}

static int utf8_cp(const unsigned char* s)
{
    if (s[0] < 0x80) return s[0];
    if ((s[0] & 0xE0) == 0xC0) return ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
    if ((s[0] & 0xF0) == 0xE0) return ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    return '?';
}

static void put_char708(int cp)
{
    uint8_t u[2];
    if (cp >= 0x20 && cp < 0x7F) { u[0] = cp; unit(u, 1); }
    else if (cp >= 0xA0 && cp <= 0xFF) { u[0] = cp; unit(u, 1); }  /* G1 = Latin-1 */
    else if (cp == 0x266A) { u[0] = 0x7F; unit(u, 1); }            /* music note */
    else {
        int g2 = cp == 0x2122 ? 0x39 : cp == 0x2018 ? 0x31 : cp == 0x2019 ? 0x32 : cp == 0x201C ? 0x33 : cp == 0x201D ? 0x34 : -1;
        if (g2 >= 0) { u[0] = 0x10; u[1] = g2; unit(u, 2); }      /* EXT1 + G2 */
        else { u[0] = '?'; unit(u, 1); }
    }
}

typedef struct { int cp[SCREEN_COLS]; } row_t;
static row_t prev_rows[SCREEN_ROWS];
static int prev_valid = 0;

/* Turn the current 608 front buffer into a 708 window redraw */
static void update_708(caption_frame_t* frame)
{
    row_t rows[SCREEN_ROWS];
    int r0 = -1, r1 = -1;
    for (int r = 0; r < SCREEN_ROWS; r++) {
        for (int c = 0; c < SCREEN_COLS; c++) {
            const utf8_char_t* ch = caption_frame_read_char(frame, r, c, NULL, NULL);
            rows[r].cp[c] = (ch && ch[0]) ? utf8_cp((const unsigned char*)ch) : 0;
            if (rows[r].cp[c] && rows[r].cp[c] != ' ') {
                if (r0 < 0) r0 = r;
                r1 = r;
            }
        }
    }
    if (prev_valid && !memcmp(rows, prev_rows, sizeof rows))
        return; /* nothing changed on screen */
    memcpy(prev_rows, rows, sizeof rows);
    prev_valid = 1;

    pack_begin();
    { uint8_t u[] = { 0x8C, 0xFF }; unit(u, 2); } /* DeleteWindows 0-7 */
    if (r0 >= 0) {
        uint8_t df[7] = { 0x98,                    /* DefineWindow 0                     */
            0x38,                                  /* visible, row+col lock, priority 0  */
            (uint8_t)(0x80 | (r0 * 100 / SCREEN_ROWS)), /* relative anchor: vertical %  */
            50,                                    /* horizontal % (centered)            */
            (uint8_t)(0x10 | (r1 - r0)),           /* anchor pt 1 (top-centre), rows-1   */
            SCREEN_COLS - 1,                       /* columns-1                          */
            0x09 };                                /* window style 1, pen style 1 (NTSC) */
        unit(df, 7);
        for (int r = r0; r <= r1; r++) {
            int c0 = -1, c1 = -1;
            for (int c = 0; c < SCREEN_COLS; c++)
                if (rows[r].cp[c] && rows[r].cp[c] != ' ') { if (c0 < 0) c0 = c; c1 = c; }
            if (c0 < 0) continue;
            uint8_t spl[3] = { 0x92, (uint8_t)(r - r0), (uint8_t)c0 }; /* SetPenLocation */
            unit(spl, 3);
            for (int c = c0; c <= c1; c++)
                put_char708(rows[r].cp[c] ? rows[r].cp[c] : ' ');
        }
        { uint8_t etx = 0x03; unit(&etx, 1); } /* ETX: end of text; decoders (e.g. VLC) render on it */
    }
    pack_end();
}

/* ------------------------------------------------------------------------- */
/* 3. Per-frame cc_data assembly                                              */
/* ------------------------------------------------------------------------- */

static int cc_count = 20; /* CEA-708 figure for 29.97 fps */
static caption_frame_t cframe;
static int want708 = 1;
static int f2mode = 0;   /* 0 = invalid slot F9 00 00, 1 = valid null pair FD 80 80, 2 = omit slot */
static int fillmode = 0; /* 0 = invalid filler FA 00 00, 1 = valid FE 00 00, 2 = none (variable cc_count) */

static int build_cc_data(long n, uint8_t* out)
{
    uint16_t p = (size_t)n < sched_len ? sched[n] : 0;
    int o = 0;
    if (p && want708 && LIBCAPTION_READY == caption_frame_decode(&cframe, p, (double)n))
        update_708(&cframe);

    uint16_t f1 = p ? p : 0x8080;
    out[o++] = 0xFC; out[o++] = f1 >> 8; out[o++] = f1 & 0xFF;  /* 608 field 1 (valid) */
    if (f2mode == 0) { out[o++] = 0xF9; out[o++] = 0x00; out[o++] = 0x00; }      /* field 2 invalid */
    else if (f2mode == 1) { out[o++] = 0xFD; out[o++] = 0x80; out[o++] = 0x80; } /* field 2 valid null */
    int slots = cc_count - (f2mode == 2 ? 1 : 2);
    for (int i = 0; i < slots; i++) {
        if (want708 && q_head < q_len) {
            trip_t t = q[q_head++];
            out[o++] = 0xF8 | 0x04 | t.type; /* marker bits + cc_valid + type (FE / FF) */
            out[o++] = t.a; out[o++] = t.b;
        } else if (fillmode == 2) {
            break;
        } else if (fillmode == 1) {
            out[o++] = 0xFE; out[o++] = 0x00; out[o++] = 0x00; /* valid, empty DTVCC data */
        } else {
            out[o++] = 0xFA; out[o++] = 0x00; out[o++] = 0x00; /* invalid DTVCC filler */
        }
    }
    return o;
}

/* ------------------------------------------------------------------------- */
/* 3b. Progress reporting                                                     */
/* ------------------------------------------------------------------------- */

static void report_progress(long n, time_t t0, int final)
{
    static time_t last_print = 0;
    time_t now = time(NULL);
    int is_tty = isatty(STDERR_FILENO);
    if (!final && !is_tty && now == last_print)
        return; /* redirected to a file: at most one line per second */
    if (!final && is_tty && now == last_print)
        return; /* terminal: also throttle to ~1/s, cheap and legible */
    last_print = now;

    double elapsed = difftime(now, t0);
    double fps = elapsed > 0 ? n / elapsed : 0;
    long tc_f = n % 30, tc_s = (n / 30) % 60, tc_m = (n / 1800) % 60, tc_h = n / 108000;

    if (is_tty)
        fprintf(stderr, "\rframe=%6ld  time=%02ld:%02ld:%02ld:%02ld  elapsed=%.0fs  speed=%.1ffps  ",
            n, tc_h, tc_m, tc_s, tc_f, elapsed, fps);
    else
        fprintf(stderr, "frame=%ld time=%02ld:%02ld:%02ld:%02ld elapsed=%.0fs speed=%.1ffps\n",
            n, tc_h, tc_m, tc_s, tc_f, elapsed, fps);
    if (final && is_tty)
        fprintf(stderr, "\n");
    fflush(stderr);
}

/* ------------------------------------------------------------------------- */
/* 4. Encode                                                                  */
/* ------------------------------------------------------------------------- */

int main(int argc, char** argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s in.scc out.ts [--size WxH] [--bitrate bps] [--tc-start HH:MM:SS;FF] [--bf N] [--f2 invalid|null|none] [--filler invalid|valid|none] [--no708] [--cc-count N]\n  raw yuv420p on stdin, 30000/1001 fps\n", argv[0]);
        return 1;
    }
    int W = 1920, H = 1080, bframes = 0;
    int64_t br = 15000000;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &W, &H);
        else if (!strcmp(argv[i], "--bitrate") && i + 1 < argc) br = atoll(argv[++i]);
        else if (!strcmp(argv[i], "--no708")) want708 = 0;
        else if (!strcmp(argv[i], "--bf") && i + 1 < argc) bframes = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--f2") && i + 1 < argc) { char* v = argv[++i]; f2mode = !strcmp(v, "null") ? 1 : !strcmp(v, "none") ? 2 : 0; }
        else if (!strcmp(argv[i], "--filler") && i + 1 < argc) { char* v = argv[++i]; fillmode = !strcmp(v, "valid") ? 1 : !strcmp(v, "none") ? 2 : 0; }
        else if (!strcmp(argv[i], "--tc-start") && i + 1 < argc) { /* e.g. 01:00:00;00 */
            int h, m, s2, f2; char sp;
            if (5 == sscanf(argv[++i], "%d:%d:%d%c%d", &h, &m, &s2, &sp, &f2))
                tc_start_frame = tc_to_frame(h, m, s2, f2, sp == ';');
        }
        else if (!strcmp(argv[i], "--cc-count") && i + 1 < argc) cc_count = atoi(argv[++i]);
    }
    if (cc_count < 2) cc_count = 2;
    if (cc_count > 31) cc_count = 31;

    if (load_scc(argv[1]) < 0)
        return 1;
    caption_frame_init(&cframe);

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
    AVCodecContext* c = avcodec_alloc_context3(codec);
    c->width = W; c->height = H;
    c->time_base = (AVRational) { 1001, 30000 };
    c->framerate = (AVRational) { 30000, 1001 };
    c->pix_fmt = AV_PIX_FMT_YUV420P;
    c->sample_aspect_ratio = (AVRational) { 1, 1 };
    c->bit_rate = br; c->rc_max_rate = br; c->rc_min_rate = br;
    c->rc_buffer_size = 9781248;                 /* MP@HL / High@High VBV limit */
    c->gop_size = 15; c->max_b_frames = bframes;       /* --bf 2 gives M=3 like the target; 0 (default) avoids caption reordering issues in players */
    c->profile = MPEG2_PROFILE_HIGH;          /* High@High */
    c->level = 4;
    av_opt_set(c, "gop_timecode", "00:00:00;00", AV_OPT_SEARCH_CHILDREN); /* drop-frame GOP timecodes */
    av_opt_set_int(c, "a53cc", 1, AV_OPT_SEARCH_CHILDREN);
    if (avcodec_open2(c, codec, NULL) < 0) {
        fprintf(stderr, "cannot open mpeg2video encoder\n");
        return 1;
    }

    /* Write packets through libavformat (container chosen by extension, e.g. .mkv) so the
     * encoder's PTS/DTS survive. A raw .m2v ES loses I/P timestamps on remux, which breaks
     * caption reordering in players such as VLC. */
    AVFormatContext* oc = NULL;
    avformat_alloc_output_context2(&oc, NULL, NULL, argv[2]);
    if (!oc) { fprintf(stderr, "cannot guess output format for %s\n", argv[2]); return 1; }
    AVStream* ost = avformat_new_stream(oc, NULL);
    avcodec_parameters_from_context(ost->codecpar, c);
    ost->time_base = c->time_base;
    ost->avg_frame_rate = c->framerate;
    if (!(oc->oformat->flags & AVFMT_NOFILE) && avio_open(&oc->pb, argv[2], AVIO_FLAG_WRITE) < 0) {
        fprintf(stderr, "cannot open %s\n", argv[2]);
        return 1;
    }
    if (avformat_write_header(oc, NULL) < 0) { fprintf(stderr, "write_header failed\n"); return 1; }
    AVPacket* pkt = av_packet_alloc();
    size_t ysz = (size_t)W * H, csz = ((size_t)W / 2) * (H / 2);
    uint8_t* raw = malloc(ysz + 2 * csz);
    long n = 0;
    time_t t0 = time(NULL);

    for (;;) {
        int eof = fread(raw, 1, ysz + 2 * csz, stdin) != ysz + 2 * csz;
        AVFrame* f = NULL;
        if (!eof) {
            const uint8_t* src[4];
            int sl[4];
            av_image_fill_arrays((uint8_t**)src, sl, raw, c->pix_fmt, W, H, 1);
            f = av_frame_alloc();
            f->format = c->pix_fmt; f->width = W; f->height = H;
            av_frame_get_buffer(f, 0);
            av_image_copy(f->data, f->linesize, src, sl, c->pix_fmt, W, H);
            f->pts = n;

            uint8_t cc[31 * 3];
            int sz = build_cc_data(n, cc);
            AVFrameSideData* sd = av_frame_new_side_data(f, AV_FRAME_DATA_A53_CC, sz);
            memcpy(sd->data, cc, sz);
            n++;
            report_progress(n, t0, 0);
        }
        if (avcodec_send_frame(c, f) < 0)
            break;
        av_frame_free(&f);
        while (avcodec_receive_packet(c, pkt) == 0) {
            av_packet_rescale_ts(pkt, c->time_base, ost->time_base);
            pkt->stream_index = ost->index;
            av_interleaved_write_frame(oc, pkt);
        }
        if (eof)
            break;
    }
    av_write_trailer(oc);
    if (!(oc->oformat->flags & AVFMT_NOFILE)) avio_closep(&oc->pb);
    report_progress(n, t0, 1);
    fprintf(stderr, "encoded %ld frames; %zu DTVCC triplets left unsent in queue\n", n, q_len - q_head);
    return 0;
}
