#define MINIAUDIO_IMPLEMENTATION
#include "../libraries/miniaudio.h"
#include <GL/gl.h>

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Gl_Window.H>
#include <FL/Fl_Scrollbar.H>

#include <vector>
#include <iostream>
#include <cmath>
#include <algorithm>


class WaveformView : public Fl_Gl_Window {
public:

    WaveformView(int X, int Y, int W, int H)
        : Fl_Gl_Window(X, Y, W, H) {
        end();
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
        redraw();
    }

    void setScrollbar(Fl_Scrollbar* sb) {
        scrollbar = sb;
        updateScrollbar();
    }

    void updateScrollbar() {

        if (!scrollbar || samples.empty()) {
            return;
        } 

        int visibleSamples = static_cast<int>(w() / zoomLevel);
        int maxOffset = std::max(0, (int)samples.size() - visibleSamples);
        scrollOffset = std::clamp(scrollOffset, 0, maxOffset);

        scrollbar->maximum(maxOffset);
        scrollbar->value(scrollOffset);
        scrollbar->slider_size((float)visibleSamples / samples.size());
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

        if (samples.empty()) {
            return;
        }

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
                int endSample   = scrollOffset + static_cast<int>((x + 1) * samplesPerPixel);

                if (startSample >= (int)samples.size()) break;
                endSample = std::min(endSample, (int)samples.size());

                float minY = 1.0f;
                float maxY = -1.0f;

                for (int i = startSample; i < endSample; ++i) {
                    float s = samples[i];
                    if (s < minY) minY = s;
                    if (s > maxY) maxY = s;
                }

                // Avoid disappearing lines: pad very flat sections
                if (std::abs(maxY - minY) < 0.01f) {
                    minY -= 0.005f;
                    maxY += 0.005f;
                }

                // Clamp to [-1, 1] to stay inside viewport.
                minY = std::clamp(minY, -1.0f, 1.0f);
                maxY = std::clamp(maxY, -1.0f, 1.0f);

                glVertex2f(x, minY);
                glVertex2f(x, maxY);
            }

            glEnd();
        }
        else {
            // ZOOMED IN: One sample per vertex, smooth line.
            glBegin(GL_LINE_STRIP);

            int totalSamples = samples.size();
            // Note: Add +1 sample to visible range to ensure last visible pixel is drawn.
            int visibleSamples = static_cast<int>(std::ceil(w() / zoomLevel)) + 1;
            int endSample = std::min(scrollOffset + visibleSamples, totalSamples);

            for (int i = scrollOffset; i < endSample; ++i) {
                float x = (i - scrollOffset) * zoomLevel;
                float y = std::clamp(samples[i], -1.0f, 1.0f);  // stay in range
                glVertex2f(x, y);
            }

            glEnd();

            // --- Draw nodes if zoomed in enough ---
            float samplesPerPixel = 1.0f / zoomLevel;
            if (samplesPerPixel <= 0.1f) {
                glColor3f(1.0f, 0.0f, 0.0f); // red nodes
                glPointSize(4.0f); // make points more visible
                glBegin(GL_POINTS);

                for (int i = scrollOffset; i < endSample; ++i) {
                    float x = (i - scrollOffset) * zoomLevel;
                    float y = std::clamp(samples[i], -1.0f, 1.0f);
                    glVertex2f(x, y);
                }

                glEnd();
            }
        }
    }

    int handle(int event) override {
        switch (event) {
            case FL_MOUSEWHEEL: {
                // Zoom with mouse wheel
                if (Fl::event_dy() < 0) {
                    // Zoom in.
                    zoomLevel *= 1.1f;  
                }
                else {
                    // Zoom out.
                    zoomLevel *= 0.9f;  
                }

                zoomLevel = std::clamp(zoomLevel, zoomMin, 100.0f);

                int visibleSamples = static_cast<int>(w() / zoomLevel);
                int maxOffset = std::max(0, (int)samples.size() - visibleSamples);
                scrollOffset = std::clamp(scrollOffset, 0, maxOffset);

                updateScrollbar();
                redraw();

                return 1;
            }
            default:
                return Fl_Gl_Window::handle(event);
        }
    }

private:

    std::vector<float> samples;
    int scrollOffset = 0;
    Fl_Scrollbar* scrollbar = nullptr;
    // Pixels per sample.
    float zoomLevel = 1.0f; 
    // Auto-calculated minimum zoom (fit to screen).
    float zoomMin = 0.01f;      
};

bool loadWavToMono(const char* filename, std::vector<float>& outSamples) {
    ma_result result;
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
    ma_decoder decoder;

    result = ma_decoder_init_file(filename, &config, &decoder);
    if (result != MA_SUCCESS) {
        std::cerr << "Failed to load file: " << filename << "\n";
        return false;
    }

    ma_uint64 totalFrames = 0;
    if (ma_decoder_get_length_in_pcm_frames(&decoder, &totalFrames) != MA_SUCCESS) {
        std::cerr << "Failed to get audio length.\n";
        ma_decoder_uninit(&decoder);
        return false;
    }

    std::vector<float> interleaved(totalFrames * decoder.outputChannels);
    ma_uint64 framesRead = 0;
    if (ma_decoder_read_pcm_frames(&decoder, interleaved.data(), totalFrames, &framesRead) != MA_SUCCESS) {
        std::cerr << "Failed to read audio.\n";
        ma_decoder_uninit(&decoder);
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

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: ./waveform_viewer file.wav\n";
        return 1;
    }

    std::vector<float> samples;
    if (!loadWavToMono(argv[1], samples)) {
        return 1;
    }

    Fl_Window win(800, 400, "Waveform Viewer");
    WaveformView* waveform = new WaveformView(10, 10, 780, 280);

    Fl_Scrollbar* scrollbar = new Fl_Scrollbar(10, 295, 780, 15);
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

    win.end();
    win.show();

    return Fl::run();
}

