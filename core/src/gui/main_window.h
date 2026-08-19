#pragma once
#include <imgui/imgui.h>
#include <fftw3.h>
#include <dsp/types.h>
#include <dsp/stream.h>
#include <signal_path/vfo_manager.h>
#include <string>
#include <utils/event.h>
#include <utils/arrays.h>
#include <mutex>
#include <gui/tuner.h>

#define WINDOW_FLAGS ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
#define DEFAULT_SAMPLE_RATE	8000000

class MainWindow{
public:
    virtual void init();
    virtual void end() {}
    virtual void preDraw(ImGui::WaterfallVFO* *vfo);
    virtual void draw();
    void drawUpperLine(ImGui::WaterfallVFO* vfo);
    void updateZoom() {
        this->updateWaterfallZoomBandwidth(bw);
    }
    bool sdrIsRunning();

    // Notices a source that has stopped producing samples while the radio still
    // thinks it is running, and stops it. Called once a frame from draw().
    void checkSourceAlive();
    // Millis at which play was last pressed, so the watchdog above does not count the
    // gap before the first sample as silence.
    long long playStartTime = 0;
    // How many times the source has dropped out since the program started. Once it is
    // more than the odd one, the message says something more useful than the first one
    // did - see checkSourceAlive.
    int sourceDropCount = 0;

    void performDetectedLLMAction(const std::string &whisperResult, std::string command);

    static float* acquireFFTBuffer(void* ctx);
    static void releaseFFTBuffer(void* ctx);

    // TODO: Replace with it's own class
    void setVFO(double freq);

    void setPlayState(bool _playing);
    bool isPlaying();

    bool lockWaterfallControls = false;
    bool playButtonLocked = false;
    bool spacePressed = false;

    Event<bool> onPlayStateChange;
    Event<ImGuiContext *> onWaterfallDrawn;
    Event<ImGuiContext *> onDebugDraw;

    void setFirstMenuRender() {
        firstMenuRender = true;
    }

    void addBottomWindow(std::string name, std::function<void()> drawFunc);
    void removeBottomWindow(std::string name);
    void updateBottomWindowLayout();
    void drawBottomWindows(int dy);
    bool hasBottomWindow(std::string name);

    bool logWindow = false;
    bool showMenu = true;

    // Mic stream handling
    dsp::stream<dsp::stereo_t> micStream;
    std::shared_ptr<std::thread> micThread;
    std::atomic<bool> micThreadRunning = false;
    std::mutex micSamplesMutex;

    // Main thread task queue
    std::vector<std::function<void()>> mainThreadTasks;
    std::mutex mainThreadTasksMutex;

    // Add a task to be executed on the main thread
    void addMainThreadTask(std::function<void()> task) {
        std::lock_guard<std::mutex> lock(mainThreadTasksMutex);
        mainThreadTasks.push_back(task);
    }

    // Drains the queue above. Every layout has to call this once a frame: the
    // transceiver layout does not go through MainWindow::draw(), so tasks
    // queued from other threads used to sit there forever.
    void runMainThreadTasks();


protected:
    void displayVariousWindows();
    static void vfoAddedHandler(VFOManager::VFO* vfo, void* ctx);

    // FFT Variables
    int fftSize = 8192 * 8;
    std::mutex fft_mtx;
//    fftwf_complex *fft_in, *fft_out;
//    fftwf_plan fftwPlanImplFFTW;
    //dsp::arrays::Arg<dsp::arrays::fftwPlanImplFFTW> waterfallPlan;

    // GUI Variables
    bool firstMenuRender = true;
    bool startedWithMenuClosed = false;
    float fftMin = -70.0;
    float fftMax = 0.0;
    float bw = 1.0;             // slider position 0.0 .. 1.0
    bool playing = false;
    std::string audioStreamName = "";
    std::string sourceName = "";
    int menuWidth = 300;
    bool grabbingMenu = false;
    int newWidth = 300;
    int fftHeight = 300;
    int tuningMode = tuner::TUNER_MODE_NORMAL;

    dsp::stream<dsp::complex_t> dummyStream;
    bool demoWindow = false;
    int selectedWindow = 0;

    bool initComplete = false;
    bool autostart = false;

    EventHandler<VFOManager::VFO*> vfoCreatedHandler;

    void updateWaterfallZoomBandwidth(float bw);
    void handleWaterfallInput(ImGui::WaterfallVFO* vfo);

    struct ButtomWindow {
        std::string name;
        std::function<void()> drawFunc;
        ImVec2 loc;
        ImVec2 size;
    };

    std::vector<ButtomWindow> bottomWindows;

    void drawDebugMenu();

    void ShowLogWindow();
};

#ifdef __ANDROID__

#include <android_backend.h>

#endif
