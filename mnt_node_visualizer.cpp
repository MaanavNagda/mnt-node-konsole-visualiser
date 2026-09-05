#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// build: g++ -O3 -pthread -std=c++20 -o mnt_node_visualizer mnt_node_visualizer.cpp
// ---------------------------------------------------------------------------

constexpr int SAMPLE_RATE = 48000;
constexpr int CHANNELS = 2;
constexpr int CHUNK = 1024;
constexpr long FRAME_NS = 16666666L;
constexpr float PI = 3.14159265358979323846f;

std::atomic<bool> g_running{true};
std::atomic<bool> g_resize_flag{true};
std::atomic<bool> g_muted{false};
std::atomic<float> g_bass{0.0f};
std::atomic<float> g_mid{0.0f};
std::atomic<float> g_treble{0.0f};
std::atomic<float> g_vol{0.0f};

// ---------------------------------------------------------------------------
// 1024-point iterative complex FFT for float (Cooley-Tukey, in-place)
// ---------------------------------------------------------------------------
class Fft {
    int n_;
    std::vector<int> rev_;
    std::vector<std::complex<float>> tw_;

public:
    explicit Fft(int n) : n_(n), rev_(n), tw_(n / 2) {
        int bits = 0;
        int t = n;
        while (t > 1) { t >>= 1; ++bits; }
        for (int i = 0; i < n_; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev_[i] = r;
        }
        for (int k = 0; k < n_ / 2; ++k)
            tw_[k] = std::polar(1.0f, -2.0f * PI * k / n_);
    }

    void forward(std::complex<float>* a) const {
        for (int i = 0; i < n_; ++i)
            if (i < rev_[i]) std::swap(a[i], a[rev_[i]]);
        for (int len = 2; len <= n_; len <<= 1) {
            int half = len >> 1;
            int step = n_ / len;
            for (int i = 0; i < n_; i += len) {
                for (int j = 0; j < half; ++j) {
                    std::complex<float> u = a[i + j];
                    std::complex<float> v = a[i + j + half] * tw_[j * step];
                    a[i + j] = u + v;
                    a[i + j + half] = u - v;
                }
            }
        }
    }
};

// ---------------------------------------------------------------------------
// Helpers: shell command capture, terminal, palette
// ---------------------------------------------------------------------------
std::string exec(const char* cmd) {
    FILE* f = popen(cmd, "r");
    if (!f) return "";
    std::string out;
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), f)) out.append(buf);
    pclose(f);
    return out;
}

std::string get_default_sink() {
    std::string out = exec("pactl info");
    std::size_t pos = out.find("Default Sink: ");
    if (pos == std::string::npos)
        throw std::runtime_error("Could not determine default sink.");
    pos += 14;
    std::size_t e = out.find('\n', pos);
    return out.substr(pos, e - pos);
}

void get_sink_info(const std::string& sink, bool& muted, std::string& monitor, int& vol_pct) {
    std::string out = exec("pactl list sinks");
    std::string needle = "Name: " + sink;
    std::size_t pos = out.find(needle);
    if (pos == std::string::npos)
        throw std::runtime_error("Default sink not found in pactl list sinks.");
    std::size_t end = out.find("\nSink #", pos + 1);
    if (end == std::string::npos) end = out.size();
    std::string block = out.substr(pos, end - pos);

    muted = (block.find("Mute: yes") != std::string::npos);

    std::size_t mp = block.find("Monitor Source: ");
    if (mp != std::string::npos) {
        mp += 16;
        std::size_t me = block.find('\n', mp);
        monitor = block.substr(mp, me - mp);
    } else {
        monitor = sink + ".monitor";
    }

    std::size_t pct = block.find('%');
    if (pct != std::string::npos) {
        std::size_t p = pct;
        while (p > 0 && std::isdigit(static_cast<unsigned char>(block[p - 1]))) --p;
        vol_pct = std::stoi(block.substr(p, pct - p));
    } else {
        vol_pct = 100;
    }
}

struct RenderState {
    int rows = 0;
    int cols = 0;
    int dot_w = 0;
    int dot_h = 0;
    std::vector<float> xs;
    std::vector<float> ys;
    std::vector<float> d;
    std::vector<std::uint8_t> palette;
    std::string frame;
};

void generate_palette(RenderState& rs) {
    rs.palette.resize(256 * 3);
    for (int i = 0; i < 256; ++i) {
        float h = i / 255.0f;
        int k = static_cast<int>(h * 6.0f);
        float f = h * 6.0f - k;
        float q = 1.0f - f;
        float t = f;
        float r = 0.0f, g = 0.0f, b = 0.0f;
        switch (k % 6) {
            case 0: r = 1.0f; g = t;    b = 0.0f; break;
            case 1: r = q;    g = 1.0f; b = 0.0f; break;
            case 2: r = 0.0f; g = 1.0f; b = t;    break;
            case 3: r = 0.0f; g = q;    b = 1.0f; break;
            case 4: r = t;    g = 0.0f; b = 1.0f; break;
            case 5: r = 1.0f; g = 0.0f; b = q;    break;
        }
        rs.palette[i * 3 + 0] = static_cast<std::uint8_t>(r * 255.0f);
        rs.palette[i * 3 + 1] = static_cast<std::uint8_t>(g * 255.0f);
        rs.palette[i * 3 + 2] = static_cast<std::uint8_t>(b * 255.0f);
    }
}

void resize(RenderState& rs) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0) {
        ws.ws_row = 24;
        ws.ws_col = 80;
    }
    rs.rows = ws.ws_row;
    rs.cols = ws.ws_col;
    rs.dot_w = std::max(1, rs.cols - 1);
    rs.dot_h = std::max(1, rs.rows);
    int n = rs.dot_h * rs.dot_w;
    rs.xs.resize(n);
    rs.ys.resize(n);
    rs.d.resize(n);
    float half = std::max(rs.dot_w, rs.dot_h) / 2.0f;
    if (half < 1.0f) half = 1.0f;
    for (int cy = 0; cy < rs.dot_h; ++cy) {
        float yv = (cy - (rs.dot_h - 1) * 0.5f) / half;
        for (int cx = 0; cx < rs.dot_w; ++cx) {
            float xv = (cx - (rs.dot_w - 1) * 0.5f) / half;
            int i = cy * rs.dot_w + cx;
            rs.xs[i] = xv;
            rs.ys[i] = yv;
            rs.d[i] = std::sqrt(xv * xv + yv * yv);
        }
    }
    rs.frame.clear();
    rs.frame.reserve(n * 28 + 64);
}

// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------
void sigwinch_handler(int) { g_resize_flag.store(true, std::memory_order_relaxed); }

void audio_thread(const std::string monitor) {
    std::string cmd = "parec --format=s16le --rate=" +
                      std::to_string(SAMPLE_RATE) +
                      " --channels=" + std::to_string(CHANNELS) +
                      " --device " + monitor;
    FILE* f = popen(cmd.c_str(), "r");
    if (!f) {
        std::cerr << "Failed to start parec\n";
        g_running.store(false, std::memory_order_relaxed);
        return;
    }

    const std::size_t raw_bytes = CHUNK * CHANNELS * sizeof(std::int16_t);
    std::vector<char> raw(raw_bytes);
    std::vector<float> mono(CHUNK);
    std::vector<float> window(CHUNK);
    std::vector<std::complex<float>> fftbuf(CHUNK);
    Fft fft(CHUNK);

    for (int i = 0; i < CHUNK; ++i)
        window[i] = 0.5f - 0.5f * std::cos((2.0f * PI * i) / (CHUNK - 1));

    while (g_running.load(std::memory_order_relaxed)) {
        std::size_t got = 0;
        while (got < raw_bytes) {
            std::size_t r = std::fread(raw.data() + got, 1, raw_bytes - got, f);
            if (r == 0) {
                g_running.store(false, std::memory_order_relaxed);
                break;
            }
            got += r;
        }
        if (!g_running.load(std::memory_order_relaxed)) break;

        const std::int16_t* s = reinterpret_cast<const std::int16_t*>(raw.data());
        float vol_acc = 0.0f;
        for (int i = 0; i < CHUNK; ++i) {
            float l = s[i * 2 + 0] / 32768.0f;
            float rch = s[i * 2 + 1] / 32768.0f;
            float m = (l + rch) * 0.5f;
            mono[i] = m;
            vol_acc += m * m;
            fftbuf[i] = std::complex<float>(m * window[i], 0.0f);
        }
        float vol = std::sqrt(vol_acc / CHUNK);
        vol = std::min(1.0f, vol * 4.0f);

        fft.forward(fftbuf.data());

        float total = 0.0f;
        for (int i = 0; i <= CHUNK / 2; ++i) {
            float mag = std::abs(fftbuf[i]);
            total += mag;
        }
        float inv = 1.0f / (total + 1e-9f);
        float binw = static_cast<float>(SAMPLE_RATE) / CHUNK;

        auto band = [&](float low, float high) {
            int il = std::max(0, static_cast<int>(std::ceil(low / binw)));
            int ih = std::min(CHUNK / 2, static_cast<int>(std::floor(high / binw)));
            float sum = 0.0f;
            for (int k = il; k <= ih; ++k) sum += std::abs(fftbuf[k]);
            return sum * inv;
        };

        float bass = std::min(1.0f, band(20.0f, 150.0f) * 2.5f);
        float mid = std::min(1.0f, band(150.0f, 1200.0f) * 2.5f);
        float treble = std::min(1.0f, band(1200.0f, 8000.0f) * 2.5f);

        g_bass.store(bass, std::memory_order_relaxed);
        g_mid.store(mid, std::memory_order_relaxed);
        g_treble.store(treble, std::memory_order_relaxed);
        g_vol.store(vol, std::memory_order_relaxed);
    }
    pclose(f);
}

void mute_monitor_thread(const std::string sink) {
    while (g_running.load(std::memory_order_relaxed)) {
        try {
            bool m;
            std::string mon;
            int p;
            get_sink_info(sink, m, mon, p);
            g_muted.store(m, std::memory_order_relaxed);
        } catch (...) {
            g_muted.store(true, std::memory_order_relaxed);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

void input_thread() {
    char c;
    while (g_running.load(std::memory_order_relaxed)) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        struct timeval tv = {0, 100000}; // 100 ms
        if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0) {
            if (read(STDIN_FILENO, &c, 1) == 1) {
                if (c == 'q' || c == '\x03') {
                    g_running.store(false, std::memory_order_relaxed);
                    break;
                }
            }
        }
    }
}

void render_thread() {
    RenderState rs;
    resize(rs);
    generate_palette(rs);

    std::mt19937 rng(static_cast<unsigned>(
        std::chrono::steady_clock::now().time_since_epoch().count()));

    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    next.tv_nsec += FRAME_NS;
    if (next.tv_nsec >= 1000000000L) {
        next.tv_nsec -= 1000000000L;
        next.tv_sec += 1;
    }

    float t = 0.0f;
    float phase_a = 0.0f;
    float phase_b = 0.0f;

    while (g_running.load(std::memory_order_relaxed)) {
        int ret = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr);
        if (ret == -1 && errno != EINTR) break;

        next.tv_nsec += FRAME_NS;
        if (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec += 1;
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (next.tv_sec < now.tv_sec ||
            (next.tv_sec == now.tv_sec && next.tv_nsec < now.tv_nsec)) {
            next = now;
            next.tv_nsec += FRAME_NS;
            if (next.tv_nsec >= 1000000000L) {
                next.tv_nsec -= 1000000000L;
                next.tv_sec += 1;
            }
        }

        if (g_resize_flag.exchange(false, std::memory_order_relaxed)) {
            resize(rs);
        }

        float bass = g_bass.load(std::memory_order_relaxed);
        float mid = g_mid.load(std::memory_order_relaxed);
        float treble = g_treble.load(std::memory_order_relaxed);
        float vol = g_vol.load(std::memory_order_relaxed);
        if (g_muted.load(std::memory_order_relaxed)) vol = 0.0f;

        float motion = bass + mid + treble;
        float dt = (1.0f / 60.0f) * (1.0f + 0.5f * motion);

        float tb = t * (1.0f + bass * 2.0f);
        float tm = t * (1.0f + mid * 2.5f);
        float tt = t * (1.0f + treble * 4.0f);
        float tn = t * (1.0f + motion);

        float A = phase_a;
        float B = phase_b;

        rs.frame.clear();
        rs.frame += "\x1b[H";

        char buf[64];
        for (int cy = 0; cy < rs.dot_h; ++cy) {
            for (int cx = 0; cx < rs.dot_w; ++cx) {
                int i = cy * rs.dot_w + cx;
                float xv = rs.xs[i];
                float yv = rs.ys[i];
                float dv = rs.d[i];

                float tb = t * (1.0f + bass * 2.0f);
                float tm = t * (1.0f + mid * 2.5f);
                float tt = t * (1.0f + treble * 4.0f);
                float tn = t * (1.0f + bass + mid + treble);

                float base = 0.4f * (
                    std::sin(xv * 12.0f + t) +
                    std::cos(yv * 10.0f + t * 1.3f) +
                    std::sin(dv * 7.0f - t * 1.6f)
                );

                float audio =
                    std::sin(xv * (10.0f + bass * 50.0f) + tb) * (bass + 0.1f) +
                    std::sin(yv * (10.0f + mid * 50.0f) + tm) * (mid + 0.1f) +
                    std::cos(dv * (22.0f + treble * 70.0f) - tt) * (treble + 0.1f) * 1.5f;

                float noise = 0.5f * (
                    std::sin(xv * 9.0f + yv * 15.0f + tn * 5.0f + A) *
                    std::cos(xv * 6.0f - yv * 11.0f - tn * 4.0f + B)
                );

                float gain = 1.0f + 0.5f * vol;
                float cval = std::abs((base + audio + noise) * gain);

                float threshold = 1.6f * (1.0f - vol);

                if (cval > threshold) {
                    float hue = std::fmod(cval * 0.3f + t * 0.08f + (cx / static_cast<float>(rs.dot_w)) * 0.12f, 1.0f);
                    if (hue < 0.0f) hue += 1.0f;
                    float brightness = std::min(1.0f, 0.3f + (cval - threshold) * 0.6f + vol * 0.2f);
                    int idx = static_cast<int>(hue * 255.0f);
                    if (idx < 0) idx = 0;
                    if (idx > 255) idx = 255;

                    std::uint8_t r = static_cast<std::uint8_t>(rs.palette[idx * 3 + 0] * brightness);
                    std::uint8_t g = static_cast<std::uint8_t>(rs.palette[idx * 3 + 1] * brightness);
                    std::uint8_t b = static_cast<std::uint8_t>(rs.palette[idx * 3 + 2] * brightness);

                    int len = std::snprintf(buf, sizeof(buf), "\x1b[48;2;%d;%d;%dm ", r, g, b);
                    rs.frame.append(buf, static_cast<std::size_t>(len));
                } else {
                    int len = std::snprintf(buf, sizeof(buf), "\x1b[0m ");
                    rs.frame.append(buf, static_cast<std::size_t>(len));
                }
            }
            rs.frame += cy == rs.dot_h - 1 ? "\x1b[0m" : "\x1b[0m\n";
        }

        const char* data = rs.frame.data();
        std::size_t left = rs.frame.size();
        std::size_t written = 0;
        while (left > 0) {
            ssize_t w = ::write(STDOUT_FILENO, data + written, left);
            if (w <= 0) break;
            written += static_cast<std::size_t>(w);
            left -= static_cast<std::size_t>(w);
        }

        t += dt;
        if (t > 2.0f * PI * 1000.0f) t = 0.0f;
        phase_a += (bass * 2.0f + 0.5f) * dt;
        if (phase_a > 2.0f * PI * 1000.0f) phase_a = 0.0f;
        phase_b += (treble * 2.5f + 0.5f) * dt;
        if (phase_b > 2.0f * PI * 1000.0f) phase_b = 0.0f;
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main() {
    try {
        std::string sink = get_default_sink();
        bool muted = false;
        std::string monitor;
        int vol_pct = 100;
        get_sink_info(sink, muted, monitor, vol_pct);
        g_muted.store(muted, std::memory_order_relaxed);

        termios old_t, new_t;
        if (tcgetattr(STDIN_FILENO, &old_t) < 0)
            throw std::runtime_error("tcgetattr failed");
        new_t = old_t;
        new_t.c_lflag &= ~(ICANON | ECHO | ISIG);
        tcsetattr(STDIN_FILENO, TCSANOW, &new_t);

        const char init[] = "\x1b[?1049h\x1b[?25l\x1b]0;MNT Node Konsole Visualiser\x07";
        ::write(STDOUT_FILENO, init, sizeof(init) - 1);

        signal(SIGWINCH, sigwinch_handler);

        std::thread audio(audio_thread, monitor);
        std::thread mute_mon(mute_monitor_thread, sink);
        std::thread input(input_thread);
        std::thread render(render_thread);

        input.join();
        g_running.store(false, std::memory_order_release);
        render.join();

        const char restore[] = "\x1b[?1049l\x1b[?25h";
        ::write(STDOUT_FILENO, restore, sizeof(restore) - 1);
        tcsetattr(STDIN_FILENO, TCSADRAIN, &old_t);

        audio.detach();
        mute_mon.detach();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
