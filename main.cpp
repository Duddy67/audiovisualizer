#define MINIAUDIO_IMPLEMENTATION
#include "../libraries/miniaudio.h"
#include <GL/gl.h>
#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Gl_Window.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Group.H>
#include <vector>
#include <iostream>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>


// Forward class declarations.
class Audio;
class WaveformView;

// ---- Context Struct ----
struct AppContext {
    Audio* audio;
    WaveformView* view;
    Fl_Button* playBtn;
    Fl_Button* stopBtn;
};

// Forward function declarations.
void play(AppContext* ctx);
void stop(AppContext* ctx);
void pause(AppContext* ctx);
void resetCursor(AppContext* ctx);

// ---- Audio Class ----
class Audio {
public:
    std::vector<float> leftSamples;
    std::vector<float> rightSamples;
    std::atomic<int> playbackSampleIndex{0};
    int totalSamples = 0;
    // Update this based on the actual file.
    int sampleRate = 44100;
    ma_device device;
    // End of file flag.
    bool eof = false;

    bool init(const std::vector<float>& left, const std::vector<float>& right, int rate);

    void start() {
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
    int framesToCopy = std::min((int)frameCount, remaining);

    // Check for end of file.
    if (remaining == 0 && !self->eof) {
        self->eof = true;
    }

    // Fill buffer with interleaved stereo samples
    for (int i = 0; i < framesToCopy; ++i) {
        int idx = self->playbackSampleIndex++;
        // Left
        out[i * 2]     = self->leftSamples[idx];   
        // Right
        out[i * 2 + 1] = self->rightSamples[idx];  
    }

    // Fill remaining frames with silence
    for (int i = framesToCopy; i < (int)frameCount; ++i) {
        out[i * 2]     = 0.0f;
        out[i * 2 + 1] = 0.0f;
    }
}


bool Audio::init(const std::vector<float>& left, const std::vector<float>& right, int rate)
{
    // Store the provided audio samples into class members.
    leftSamples = left;
    rightSamples = right;
    // Assuming left and right channels are the same length.
    totalSamples = static_cast<int>(leftSamples.size());
    sampleRate = rate;

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = 2;
    config.sampleRate = sampleRate;
    config.dataCallback = audio_data_callback;
    config.pUserData = this;

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

    void setStereoSamples(const std::vector<float>& left, const std::vector<float>& right) {
        leftSamples = left;
        rightSamples = right;

        // Crude check, or pass in a flag.
        isStereo = (left != right);  

        // Fit entire waveform on screen initially.
        if (!leftSamples.empty()) {
            // Compute fit-to-screen zoom (pixels per sample that fits entire file).
            zoomFit = static_cast<float>(w()) / static_cast<float>(leftSamples.size());
            // Allow zooming out beyond fit-to-screen.
            // Note: Tweak factor (0.01 = 100× smaller than fit).
            zoomMin = zoomFit * 0.01f;   

            if (zoomMax <= zoomMin) {
                // Fallback if zoomMax wasn't sensible.
                zoomMax = zoomMin * 100.0f;    
            }

            // Start at fit-to-screen.
            zoomLevel = zoomFit;
        }
        else {
            zoomLevel = 1.0f;
            zoomFit = zoomMin = 1.0f;
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
        if (!scrollbar || leftSamples.empty()) return;
        int visibleSamples = static_cast<int>(w() / zoomLevel);
        int maxOffset = std::max(0, (int)leftSamples.size() - visibleSamples);
        scrollOffset = std::clamp(scrollOffset, 0, maxOffset);
        scrollbar->maximum(maxOffset);
        scrollbar->value(scrollOffset);
        scrollbar->slider_size((float)visibleSamples / leftSamples.size());
    }

    // Getters.

    int getScrollOffset() const { return scrollOffset; }
    float getZoomLevel() const { return zoomLevel; }
    bool isPlaying() const { return playing; }
    bool isPaused() const { return paused; }
    int getPlaybackSample() const { return playbackSample; }

    // Setters.

    void setContext(AppContext* ctx) { this->ctx = ctx; }
    int getMovedCursorSample() const { return movedCursorSample; }
    void setPlaying(bool state) { playing = state; }
    void setPaused(bool state) { paused = state; }
    void setPlaybackSample(int sample) {
        playbackSample = sample;
        redraw();
    }
    void setStereoMode(bool stereo) { isStereo = stereo; }

protected:
    void draw() override {
        if (!valid()) {
            glLoadIdentity();
            glViewport(0, 0, w(), h());
            // X: pixels, Y: normalized amplitude.
            // Top to bottom pixel coordinates
            glOrtho(0, w(), 0, h(), -1.0, 1.0);  
        }

        int halfHeight = h() / 2;

        // White background.
        glClearColor(1, 1, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT);

        if (leftSamples.empty()) return;


        // Blue waveform.
        glColor3f(0.0f, 0.0f, 1.0f);
        // Ensure full-pixel lines.
        glLineWidth(1.0f);

        // Lambda function that draws a channel.
        auto drawChannel = [&](const std::vector<float>& channel, int yOffset, int heightPx) {
            float samplesPerPixel = 1.0f / zoomLevel;

            // Decide rendering mode based on zoom level.
            if (samplesPerPixel > 5.0f) {
                // ZOOMED OUT: Envelope (min/max per pixel column)
                glBegin(GL_LINES);

                for (int x = 0; x < w(); ++x) {
                    int startSample = scrollOffset + static_cast<int>(x * samplesPerPixel);
                    int endSample = std::min(scrollOffset + static_cast<int>((x + 1) * samplesPerPixel), (int)channel.size());

                    float minY = 1.0f, maxY = -1.0f;
                    for (int i = startSample; i < endSample; ++i) {
                        float s = channel[i];
                        minY = std::min(minY, s);
                        maxY = std::max(maxY, s);
                    }

                    bool isSilent = true;

                    for (int i = startSample; i < endSample; ++i) {
                        // Noise threshold
                        if (std::abs(channel[i]) > 0.005f) {  
                            isSilent = false;
                            break;
                        }
                    }

                    if (isSilent) {
                        // Flat silent section → draw a thin horizontal line
                        float yFlatPx = yOffset + (1.0f - 0.0f) * (heightPx / 2.0f);  // Amplitude 0

                        glVertex2f(x, yFlatPx);
                        // 1-pixel wide horizontal line.
                        glVertex2f(x + 1, yFlatPx);  
                        // Skip the rest of loop.
                        continue;  
                    }

                    // Avoid disappearing lines: pad very flat sections
                    // Note: Near-flat, but not completely silent → pad it
                    if (std::abs(maxY - minY) < 0.01f) {
                        minY -= 0.005f; maxY += 0.005f;
                    }

                    float yMinPx = yOffset + (1.0f - std::clamp(minY, -1.0f, 1.0f)) * (heightPx / 2.0f);
                    float yMaxPx = yOffset + (1.0f - std::clamp(maxY, -1.0f, 1.0f)) * (heightPx / 2.0f);

                    glVertex2f(x, yMinPx);
                    glVertex2f(x, yMaxPx);
                }
                glEnd();
            }
            else {
                // ZOOMED IN: One sample per vertex, smooth line.
                glBegin(GL_LINE_STRIP);

                // Note: Add +1 sample to visible range to ensure last visible pixel is drawn.
                int visibleSamples = static_cast<int>(std::ceil(w() / zoomLevel)) + 1;
                int endSample = std::min(scrollOffset + visibleSamples, (int)channel.size());

                for (int i = scrollOffset; i < endSample; ++i) {
                    float x = (i - scrollOffset) * zoomLevel;
                    float y = yOffset + (1.0f - std::clamp(channel[i], -1.0f, 1.0f)) * (heightPx / 2.0f);
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
                        float y = yOffset + (1.0f - std::clamp(channel[i], -1.0f, 1.0f)) * (heightPx / 2.0f);
                        glVertex2f(x, y);
                    }

                    glEnd();
                }
            }
        };

        // If waveform doesn't fill the full width, paint the rest in grey
        int totalSamples = leftSamples.size();
        int visibleSamples = visibleSamplesCount();
        int endSample = scrollOffset + visibleSamples;
        // compute last drawn x position
        float lastX = (float)(std::min(endSample, totalSamples) - scrollOffset) * zoomLevel;

        if (lastX < (float)w()) {
            glBegin(GL_QUADS);
                // grey background
                glColor3f(0.3f, 0.3f, 0.3f); 
                // top-right
                glVertex2f((float)w(), (float)h()); 
                // top-left
                glVertex2f(lastX, (float)h()); 
                // bottom-left
                glVertex2f(lastX, 0.0f);       
                // bottom-right
                glVertex2f((float)w(), 0.0f);       
            glEnd();
        }

        // Waveform color (blue).
        glColor3f(0.0f, 0.0f, 1.0f); 

        if (isStereo) {
            // Draw both left and right channels.
            drawChannel(leftSamples, 0, halfHeight);
            drawChannel(rightSamples, halfHeight, halfHeight);

            // --- Draw separation line between waveforms ---

            // Dim gray
            glColor3f(0.412f, 0.412f, 0.412f); 
            glLineWidth(1.0f);
            glBegin(GL_LINES);
            // from left
            glVertex2f(0, h() / 2);     
            // to right
            glVertex2f(w(), h() / 2);   
            glEnd();

            // --- Draw zero lines (middle line) for both channels. ---

            // Gainsboro
            glColor3f(0.863f, 0.863f, 0.863f); 
            glBegin(GL_LINES);
                glLineWidth(1.0f);
                glVertex2f(0.0f,    halfHeight + (halfHeight / 2));
                glVertex2f((float)w(), halfHeight + (halfHeight / 2));
                //
                glLineWidth(1.0f);
                glVertex2f(0.0f,    halfHeight / 2.0f);
                glVertex2f((float)w(), halfHeight / 2.0f);
            glEnd();
        }
        // mono = full height
        else {
            drawChannel(leftSamples, 0, h());  
            // --- Draw zero line (middle line). ---
            glColor3f(0.863f, 0.863f, 0.863f); 
            glBegin(GL_LINES);
                glLineWidth(1.0f);
                glVertex2f(0.0f,    halfHeight);
                glVertex2f((float)w(), halfHeight);
            glEnd();
        }

        // --- Draw playback cursor ---
        int sampleToDraw = -1;

        if (isPlaying() || isPaused()) {
            // The cursor moves in realtime (isPlaying) or is shown at its last position (isPaused).
            sampleToDraw = playbackSample;
        }
        else {
            // The cursor has been manually moved (eg: mouse click, Home key...).
            sampleToDraw = movedCursorSample;
        }

        if (sampleToDraw >= 0) {
            int visibleStart = scrollOffset;
            int visibleEnd = scrollOffset + static_cast<int>(std::ceil(w() / zoomLevel));

            if (sampleToDraw >= visibleStart && sampleToDraw < visibleEnd) {
                float x = (sampleToDraw - scrollOffset) * zoomLevel;
                glColor3f(1.0f, 0.0f, 0.0f);
                glLineWidth(1.0f);
                glBegin(GL_LINES);
                glVertex2f(x, 0);
                glVertex2f(x, h());
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
                // zoom in/out
                if (Fl::event_dy() < 0) {
                    zoomLevel *= 1.1f;  // zoom in
                }
                else {
                    zoomLevel *= 0.9f;  // zoom out
                }

                zoomLevel = std::clamp(zoomLevel, zoomMin, zoomMax);

                //int visibleSamples = static_cast<int>(w() / zoomLevel);
                // re-clamp scrollOffset to keep view valid
                int visibleSamples = visibleSamplesCount();
                int maxOffset = std::max(0, (int)leftSamples.size() - visibleSamples);
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
                    sample = std::clamp(sample, 0, (int)leftSamples.size() - 1);

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

            case FL_KEYDOWN: {
                int key = Fl::event_key();

                // Spacebar: ' ' => ASCII code 32.
                if (key == ' ') {
                    if (ctx) {
                        if (isPaused() || ctx->audio->eof) {
                          stop(ctx);
                          ctx->audio->eof = false;
                        }

                        if (isPlaying()) {
                            stop(ctx);
                        }
                        else {
                            play(ctx);
                        }
                       
                    }

                    return 1;
                }
                else if (key == FL_Pause) {
                    if (ctx) {
                        pause(ctx);
                        return 1;
                    }

                    return 0;
                }
                else if (key == FL_Home) {
                    // Process only when playback is stopped.
                    if (ctx && !isPlaying()) {
                        // Reset the audio cursor to the start position.
                        movedCursorSample = 0;
                        resetCursor(ctx);

                        return 1;
                    }

                    return 0;
                }
                else if (key == FL_End) {
                    // Process only when playback is stopped.
                    if (ctx && !isPlaying()) {
                        // Take the audio cursor to the end position.
                        movedCursorSample = static_cast<int>(leftSamples.size()) - 1;
                        resetCursor(ctx);

                        return 1;
                    }

                    return 0;
                }

                return 0;
            } 

            default:
                return Fl_Gl_Window::handle(event);
        }
    }

private:
    std::vector<float> leftSamples;
    std::vector<float> rightSamples;
    Fl_Scrollbar* scrollbar = nullptr;
    // Auto-calculated minimum zoom (fit to screen).
    // Pixels per sample.
    float zoomLevel = 1.0f;
    // Fit-to-screen (current starting zoom)
    float zoomFit = 1.0f;     
    // Allow zooming out further
    float zoomMin = 1.0f;     
    // Allow up to 10 pixels per sample
    float zoomMax = 10.0f; 
    int scrollOffset = 0;
    // -1 = not playing
    int playbackSample = -1;
    bool playing = false;
    bool paused = false;
    bool isStereo = true;
    // Position of the cursor when it is manually moved.
    int movedCursorSample = 0;
    AppContext* ctx = nullptr;
    // helper to compute how many samples fit inside the widget width at current zoom
    int visibleSamplesCount() const {
        if (zoomLevel <= 0.0f) return (int)leftSamples.size();
        // number of samples that correspond to the width: ceil(w / zoomLevel)
        int vs = static_cast<int>(std::ceil(static_cast<float>(w()) / zoomLevel));
        vs = std::max(1, vs);
        vs = std::min((int)leftSamples.size(), vs);
        return vs;
    }
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
bool loadWavStereo(const std::string& path, std::vector<float>& left, std::vector<float>& right, bool& isStereo) {
    // Force float output
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);  
    ma_decoder decoder;

    if (ma_decoder_init_file(path.c_str(), &config, &decoder) != MA_SUCCESS) {
        std::cerr << "Failed to load WAV file: " << path << std::endl;
        return false;
    }

    ma_uint64 frameCount = 0;
    if (ma_decoder_get_length_in_pcm_frames(&decoder, &frameCount) != MA_SUCCESS) {
        std::cerr << "Failed to get length" << std::endl;
        ma_decoder_uninit(&decoder);
        return false;
    }

    std::vector<float> tempData(static_cast<size_t>(frameCount * decoder.outputChannels));

    ma_uint64 framesRead = 0;
    if (ma_decoder_read_pcm_frames(&decoder, tempData.data(), frameCount, &framesRead) != MA_SUCCESS) {
        std::cerr << "Failed to read PCM frames" << std::endl;
        ma_decoder_uninit(&decoder);
        return false;
    }

    ma_decoder_uninit(&decoder);

    if (decoder.outputChannels == 1) {
        // Mono data
        left = std::vector<float>(tempData);  
        // Mirror for playback
        right = left;                         
        isStereo = false;
    }
    else {
        // Split into left/right channels
        left.clear();  right.clear();
        for (size_t i = 0; i < framesRead; ++i) {
            left.push_back(tempData[i * decoder.outputChannels]);
            right.push_back(tempData[i * decoder.outputChannels + 1]);
        }

        isStereo = true;
    }

    return true;
}

void resetCursor(AppContext* ctx) 
{
    auto* view = ctx->view;
    auto* audio = ctx->audio;

    // Get the cursor's starting point.
    int resetTo = view->getMovedCursorSample();
    // Reset the cursor to its initial audio position.
    audio->playbackSampleIndex = resetTo;

    // Compute a target offset before the cursor, (e.g: show 10% of the window before the cursor.)
    float zoom = view->getZoomLevel();
    // Number of samples that fit in the view
    int visibleSamples = static_cast<int>(view->w() / zoom);
    // Shift back by a percentage of visible samples (e.g., 10%)
    int marginSamples = static_cast<int>(visibleSamples * 0.1f);
    // Compute the new scroll offset
    int newScrollOffset = std::max(0, resetTo - marginSamples);
    // Apply it.
    view->setScrollOffset(newScrollOffset);
    // Force the waveform (and cursor) to repaint
    view->redraw();  
}

void play(AppContext* ctx) 
{
    if (ctx->view->isPlaying() && ctx->audio->eof) {
        ctx->view->setPlaying(false);
    }

    if (!ctx->view->isPlaying()) {
        ctx->view->setPlaying(true);

        if (ctx->view->isPaused() || ctx->audio->eof) {
            resetCursor(ctx);
            ctx->audio->eof = false;
        }
        else {
            ctx->view->setPlaybackSample(0);
        }

        ctx->audio->start();
        Fl::add_timeout(0.016, update_cursor_timer, ctx);
    }
    else {
        resetCursor(ctx);
    }
}

void stop(AppContext* ctx) 
{
    auto* view = ctx->view;
    auto* audio = ctx->audio;

    if (view->isPlaying()) {
        view->setPlaying(false);
        ma_device_stop(&audio->device);
    }

    // Cancel possible pause state.
    view->setPaused(false);

    resetCursor(ctx);
}

void pause(AppContext* ctx) 
{
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
}

// ---- Main ----
int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: ./waveform_viewer file.wav\n";
        return 1;
    }

    std::vector<float> left;
    std::vector<float> right;
    bool isStereo;

    if (!loadWavStereo(argv[1], left, right, isStereo)) {
        std::cerr << "Failed to load WAV file.\n";
        return 1;
    }


    // Make sure each channel has the same number of audio samples.
    if (left.size() != right.size()) {
        std::cerr << "Error: Left and right channels have different lengths!" << std::endl;
        return false;
    }

    auto* audio = new Audio();
    if (!audio->init(left, right, 44100)) {
        std::cerr << "Failed to initialize audio.\n";
        return 1;
    }

    Fl_Window win(800, 400, "Waveform Viewer");
    auto* waveform = new WaveformView(10, 10, 780, 280);
    waveform->take_focus();  // Request keyboard focus
    waveform->setStereoMode(isStereo);  // update visual behavior

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
    waveform->setStereoSamples(left, right);

    Fl_Group* btns = new Fl_Group(10, 320, 240, 30);
    // Create buttons.
    Fl_Button* playBtn = new Fl_Button(10, 320, 80, 30, "Play");
    Fl_Button* stopBtn = new Fl_Button(100, 320, 80, 30, "Stop");
    Fl_Button* pauseBtn = new Fl_Button(190, 320, 80, 30, "Pause");
    btns->end();
    btns->resizable((Fl_Widget*)0); // no resizing

    auto* ctx = new AppContext{ audio, waveform, playBtn, pauseBtn };
    waveform->setContext(ctx);

    // --- Play Button ---
    playBtn->callback([](Fl_Widget*, void* userData) {
        play(static_cast<AppContext*>(userData));
    }, ctx);

    // --- Stop Button ---
    stopBtn->callback([](Fl_Widget*, void* userData) {
        stop(static_cast<AppContext*>(userData));
    }, ctx);

    // --- Pause Button ---
    pauseBtn->callback([](Fl_Widget*, void* userData) {
        pause(static_cast<AppContext*>(userData));
    }, ctx);

    // Disable keyboard focus on buttons
    playBtn->clear_visible_focus();
    stopBtn->clear_visible_focus();
    pauseBtn->clear_visible_focus();

    // Actual definition of the onSeekCallback(sample) function variable.
    ctx->view->setOnSeekCallback([ctx](int newSample) {
        ctx->audio->playbackSampleIndex = newSample;
    });

    // Make the waveform view resizable.
    win.resizable(ctx->view);
    win.end();
    win.show();
    return Fl::run();
}

