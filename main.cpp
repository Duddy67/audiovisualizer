#define MINIAUDIO_IMPLEMENTATION
#include "../libraries/miniaudio.h"
#include <GL/gl.h>
#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Gl_Window.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/Fl_Button.H>
#include <vector>
#include <iostream>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <functional>



// ---- Audio Class ----
class Audio {
public:
    std::vector<float> samples;
    std::atomic<int> playbackSampleIndex{0};
    int totalSamples = 0;
    // Update this based on the actual file.
    int sampleRate = 44100;
    ma_device device;

    bool init(const std::vector<float>& data, int rate);

    void start() {
        //playbackSampleIndex = 0;
        ma_device_start(&device);
    }

    int currentSample() const {
        return playbackSampleIndex.load();
    }
};

void audio_data_callback(ma_device* pDevice, void* output, const void*, ma_uint32 frameCount) {
    auto* self = static_cast<Audio*>(pDevice->pUserData);
    float* out = static_cast<float*>(output);

    int remaining = self->totalSamples - self->playbackSampleIndex;
    int samplesToCopy = std::min((int)frameCount, remaining);

    if (remaining <= 0) {
        // Fill silence
        std::fill(out, out + frameCount, 0.0f);
        return;
    }

    if (samplesToCopy > remaining) {
        samplesToCopy = remaining;
    }

    // Copy mono samples
    for (int i = 0; i < samplesToCopy; ++i) {
        out[i] = self->samples[self->playbackSampleIndex++];
    }

    // If less than requested, fill rest with silence
    for (int i = samplesToCopy; i < (int)frameCount; ++i) {
        out[i] = 0.0f;
    }
}

bool Audio::init(const std::vector<float>& data, int rate) {
    samples = data;
    totalSamples = static_cast<int>(samples.size());
    sampleRate = rate;

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format   = ma_format_f32;
    config.playback.channels = 1;
    config.sampleRate        = sampleRate;
    config.dataCallback      = audio_data_callback;
    config.pUserData         = this;

    return ma_device_init(nullptr, &config, &device) == MA_SUCCESS;
}


// ---- Waveform View ----
class WaveformView : public Fl_Gl_Window {
public:
    WaveformView(int X, int Y, int W, int H)
        : Fl_Gl_Window(X, Y, W, H) {
        end();
    }

    std::function<void(int)> onSeekCallback;

    void setOnSeekCallback(std::function<void(int)> callback) {
        // Assign the function variable.
        onSeekCallback = callback;
    }

    void setSamples(const std::vector<float>& data) {
        samples = data;
        // Fit entire waveform on screen initially.
        if (!samples.empty()) {
            zoomMin = static_cast<float>(w()) / samples.size();
            zoomLevel = zoomMin;
        }

        scrollOffset = 0;
        updateScrollbar();
        redraw();
    }

    void setScrollOffset(int offset) {
        scrollOffset = std::max(0, offset);
        updateScrollbar();
        redraw();
    }

    void setScrollbar(Fl_Scrollbar* sb) {
        scrollbar = sb;
        updateScrollbar();
    }

    void updateScrollbar() {
        if (!scrollbar || samples.empty()) return;
        int visibleSamples = static_cast<int>(w() / zoomLevel);
        int maxOffset = std::max(0, (int)samples.size() - visibleSamples);
        scrollOffset = std::clamp(scrollOffset, 0, maxOffset);
        scrollbar->maximum(maxOffset);
        scrollbar->value(scrollOffset);
        scrollbar->slider_size((float)visibleSamples / samples.size());
    }

    // Getters.

    int getScrollOffset() const { return scrollOffset; }
    float getZoomLevel() const { return zoomLevel; }
    bool isPlaying() const { return playing; }
    bool isPaused() const { return paused; }
    int getPlaybackSample() const { return playbackSample; }

    // Setters.

    int getMovedCursorSample() const { return movedCursorSample; }
    void setPlaying(bool state) { playing = state; }
    void setPaused(bool state) { paused = state; }
    void setPlaybackSample(int sample) {
        playbackSample = sample;
        redraw();
    }

protected:
    void draw() override {
        if (!valid()) {
            glLoadIdentity();
            glViewport(0, 0, w(), h());
            // X: pixels, Y: normalized amplitude.
            glOrtho(0, w(), -1.0, 1.0, -1.0, 1.0); 
        }

        // White background.
        glClearColor(1, 1, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT);

        if (samples.empty()) return;

        // Blue waveform.
        glColor3f(0.0f, 0.0f, 1.0f);
        // Ensure full-pixel lines.
        glLineWidth(1.0f);

        float samplesPerPixel = 1.0f / zoomLevel;

        // Decide rendering mode based on zoom level.
        if (samplesPerPixel > 5.0f) {
            // ZOOMED OUT: Envelope (min/max per pixel column)
            glBegin(GL_LINES);

            for (int x = 0; x < w(); ++x) {
                int startSample = scrollOffset + static_cast<int>(x * samplesPerPixel);
                int endSample = scrollOffset + static_cast<int>((x + 1) * samplesPerPixel);

                if (startSample >= (int)samples.size()) {
                    break;
                } 

                endSample = std::min(endSample, (int)samples.size());
                float minY = 1.0f, maxY = -1.0f;

                for (int i = startSample; i < endSample; ++i) {
                    float s = samples[i];
                    if (s < minY) minY = s;
                    if (s > maxY) maxY = s;
                }

                // Avoid disappearing lines: pad very flat sections
                if (std::abs(maxY - minY) < 0.01f) {
                    minY -= 0.005f; maxY += 0.005f;
                }

                // Clamp to [-1, 1] to stay inside viewport.
                glVertex2f(x, std::clamp(minY, -1.0f, 1.0f));
                glVertex2f(x, std::clamp(maxY, -1.0f, 1.0f));
            }
            glEnd();
        }
        else {
            // ZOOMED IN: One sample per vertex, smooth line.
            glBegin(GL_LINE_STRIP);

            // Note: Add +1 sample to visible range to ensure last visible pixel is drawn.
            int visibleSamples = static_cast<int>(std::ceil(w() / zoomLevel)) + 1;
            int endSample = std::min(scrollOffset + visibleSamples, (int)samples.size());

            for (int i = scrollOffset; i < endSample; ++i) {
                float x = (i - scrollOffset) * zoomLevel;
                // Stay in range.
                float y = std::clamp(samples[i], -1.0f, 1.0f);
                glVertex2f(x, y);
            }

            glEnd();

            // --- Draw nodes if zoomed in enough ---
            float samplesPerPixel = 1.0f / zoomLevel;

            if (samplesPerPixel <= 0.1f) {
                glColor3f(1.0f, 0.0f, 0.0f); // red nodes
                glPointSize(4.0f);           // size of each node
                glBegin(GL_POINTS);

                for (int i = scrollOffset; i < endSample; ++i) {
                    float x = (i - scrollOffset) * zoomLevel;
                    float y = std::clamp(samples[i], -1.0f, 1.0f);
                    glVertex2f(x, y);
                }

                glEnd();
            }
        }

        // --- Draw playback cursor ---
        if (playbackSample >= 0) {
            int visibleStart = scrollOffset;
            int visibleEnd = scrollOffset + static_cast<int>(std::ceil(w() / zoomLevel));

            if (playbackSample >= visibleStart && playbackSample < visibleEnd) {
                float x = (playbackSample - scrollOffset) * zoomLevel;
                glColor3f(1.0f, 0.0f, 0.0f);
                glLineWidth(1.0f);
                glBegin(GL_LINES);
                glVertex2f(x, -1.0f);
                glVertex2f(x, 1.0f);
                glEnd();
            }
        }
    }

    int handle(int event) override {
        switch (event) {
            case FL_FOCUS:
                std::cout << "Waveform got focus" << std::endl;
            return 1;

            case FL_UNFOCUS:
                std::cout << "Waveform lost focus" << std::endl;
            return 1;

            // Zoom with mouse wheel
            case FL_MOUSEWHEEL: {
                // 1.1f = zoom in / 0.9f = zoom out.
                zoomLevel *= (Fl::event_dy() < 0) ? 1.1f : 0.9f;

                zoomLevel = std::clamp(zoomLevel, zoomMin, 100.0f);

                int visibleSamples = static_cast<int>(w() / zoomLevel);
                int maxOffset = std::max(0, (int)samples.size() - visibleSamples);
                scrollOffset = std::clamp(scrollOffset, 0, maxOffset);

                updateScrollbar();
                redraw();

                return 1;
            }

            // The user has clicked and moved the cursor along the waveform.
            case FL_PUSH: {
                if (Fl::event_button() == FL_LEFT_MOUSE) {
                    int mouseX = Fl::event_x();
                    int sample = scrollOffset + static_cast<int>(mouseX / zoomLevel);

                    // Clamp within sample range
                    sample = std::clamp(sample, 0, (int)samples.size() - 1);

                    setPlaybackSample(sample);
                    movedCursorSample = sample;

                    // Update external audio playback.
                    if (onSeekCallback != nullptr) {
                         // Tell the audio system to seek too.
                        onSeekCallback(sample);  // <- call external logic
                    }

                    return 1;
                }

                return 0;
            }

            default:
                return Fl_Gl_Window::handle(event);
        }
    }

private:
    std::vector<float> samples;
    Fl_Scrollbar* scrollbar = nullptr;
    // Auto-calculated minimum zoom (fit to screen).
    float zoomMin = 0.01f;
    // Pixels per sample.
    float zoomLevel = 1.0f;
    int scrollOffset = 0;
    // -1 = not playing
    int playbackSample = -1;
    bool playing = false;
    bool paused = false;
    // Position of the cursor when it is manually moved.
    int movedCursorSample = 0;
};

// ---- Context Struct ----
struct AppContext {
    Audio* audio;
    WaveformView* view;
};

// ---- Timer Callback ----
void update_cursor_timer(void* userdata) {
    auto* ctx = static_cast<AppContext*>(userdata);
    // Reads from atomic.
    int sample = ctx->audio->currentSample();
    ctx->view->setPlaybackSample(sample);

    // --- Smart auto-scroll ---
    // Auto-scroll the view if cursor gets near right edge

    // pixels from right edge
    int margin = 30;  
    float zoom = ctx->view->getZoomLevel();
    int viewWidth = ctx->view->w();
    int cursorX = static_cast<int>((sample - ctx->view->getScrollOffset()) * zoom);

    if (cursorX > viewWidth - margin) {
        int newOffset = sample - static_cast<int>((viewWidth - margin) / zoom);
        ctx->view->setScrollOffset(newOffset);
    }

    if (sample < ctx->audio->totalSamples && ctx->view->isPlaying()) {
        // ~60 FPS
        Fl::repeat_timeout(0.016, update_cursor_timer, ctx);
    }
}

// ---- WAV Loader ----
bool loadWavToMono(const char* filename, std::vector<float>& outSamples) {
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
    ma_decoder decoder;

    if (ma_decoder_init_file(filename, &config, &decoder) != MA_SUCCESS) {
        return false;
    }

    ma_uint64 totalFrames;

    if (ma_decoder_get_length_in_pcm_frames(&decoder, &totalFrames) != MA_SUCCESS) {
        return false;
    } 

    std::vector<float> interleaved(totalFrames * decoder.outputChannels);
    ma_uint64 framesRead;

    if (ma_decoder_read_pcm_frames(&decoder, interleaved.data(), totalFrames, &framesRead) != MA_SUCCESS) {
        return false;
    }

    ma_decoder_uninit(&decoder);

    // Convert to mono
    outSamples.resize(framesRead);

    for (ma_uint64 i = 0; i < framesRead; ++i) {
        float sample = 0.0f;

        for (ma_uint32 ch = 0; ch < decoder.outputChannels; ++ch) {
            sample += interleaved[i * decoder.outputChannels + ch];
        }

        outSamples[i] = sample / decoder.outputChannels;
    }

    return true;
}

// ---- Main ----
int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: ./waveform_viewer file.wav\n";
        return 1;
    }

    std::vector<float> samples;
    if (!loadWavToMono(argv[1], samples)) {
        std::cerr << "Failed to load WAV file.\n";
        return 1;
    }

    auto* audio = new Audio();
    if (!audio->init(samples, 44100)) {
        std::cerr << "Failed to initialize audio.\n";
        return 1;
    }

    Fl_Window win(800, 400, "Waveform Viewer");
    auto* waveform = new WaveformView(10, 10, 780, 280);
    waveform->take_focus();  // Request keyboard focus

    auto* scrollbar = new Fl_Scrollbar(10, 295, 780, 15);
    scrollbar->type(FL_HORIZONTAL);
    scrollbar->step(1);
    scrollbar->minimum(0);

    scrollbar->callback([](Fl_Widget* w, void* data) {
        auto* sb = (Fl_Scrollbar*)w;
        auto* wf = (WaveformView*)data;
        wf->setScrollOffset(sb->value());
    }, waveform);

    waveform->setScrollbar(scrollbar);
    waveform->setSamples(samples);

    auto* ctx = new AppContext{ audio, waveform };

    // --- Play Button ---
    Fl_Button* playBtn = new Fl_Button(10, 320, 80, 30, "Play");

    playBtn->callback([](Fl_Widget*, void* userData) {
        auto* ctx = static_cast<AppContext*>(userData);
        if (!ctx->view->isPlaying()) {
            ctx->view->setPlaying(true);

            if (ctx->view->isPaused()) {
                int resetTo = ctx->view->getMovedCursorSample();
                ctx->audio->playbackSampleIndex = resetTo;
                // Redraw cursor to its initial position.
                ctx->view->setPlaybackSample(resetTo);  
            }
            else {
                ctx->view->setPlaybackSample(0);
            }

            ctx->audio->start();
            Fl::add_timeout(0.016, update_cursor_timer, ctx);
        }
        else {
            int resetTo = ctx->view->getMovedCursorSample();
            ctx->audio->playbackSampleIndex = resetTo;
            // Redraw cursor to its initial position.
            ctx->view->setPlaybackSample(resetTo);  
        }
    }, ctx);

    // --- Stop Button ---
    Fl_Button* stopBtn = new Fl_Button(100, 320, 80, 30, "Stop");

    stopBtn->callback([](Fl_Widget*, void* userData) {
        auto* ctx = static_cast<AppContext*>(userData);
        auto* view = ctx->view;
        auto* audio = ctx->audio;

        if (view->isPlaying()) {
            view->setPlaying(false);
            ma_device_stop(&audio->device);
        }

        // Cancel possible pause state.
        view->setPaused(false);

        int resetTo = view->getMovedCursorSample();
        audio->playbackSampleIndex = resetTo;
        // Redraw cursor to its initial position.
        view->setPlaybackSample(resetTo);  
    }, ctx);

    // --- Pause Button ---
    Fl_Button* pauseBtn = new Fl_Button(190, 320, 80, 30, "Pause");

    pauseBtn->callback([](Fl_Widget*, void* userData) {
        auto* ctx = static_cast<AppContext*>(userData);
        auto* view = ctx->view;
        auto* audio = ctx->audio;

        if (view->isPlaying()) {
            // Pause
            view->setPlaying(false);
            view->setPaused(true);
            ma_device_stop(&audio->device);
        }
        else if (view->isPaused() && !view->isPlaying()) {
            // Resume from where playback paused
            int resumeSample = view->getPlaybackSample();
            audio->playbackSampleIndex = resumeSample;
            view->setPlaying(true);
            view->setPaused(false);
            ma_device_start(&audio->device);
            Fl::add_timeout(0.016, update_cursor_timer, ctx);
        }
    }, ctx);

    // Disable keyboard focus on buttons
    playBtn->clear_visible_focus();
    stopBtn->clear_visible_focus();
    pauseBtn->clear_visible_focus();

    // Actual definition of the onSeekCallback(sample) function variable.
    ctx->view->setOnSeekCallback([ctx](int newSample) {
        ctx->audio->playbackSampleIndex = newSample;
    });


    win.end();
    win.show();
    return Fl::run();
}

