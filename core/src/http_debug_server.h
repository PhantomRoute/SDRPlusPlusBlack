#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <filesystem>
#include <functional>
#include <vector>
#include <utils/event.h>

#ifdef __cplusplus
#include "imgui.h"
#include "imgui_internal.h"
#endif

// Forward declarations for EmbeddableWebServer types
struct Server;
struct Request;
struct Connection;
struct Response;

// EWS functions - implemented in http_debug_server_impl.cpp
Server* serverInitWrapper();
void serverDeInitWrapper(Server* server);
void serverStopWrapper(Server* server);
int acceptConnectionsWrapper(Server* server, uint16_t port);

Response* responseAllocJSON(const char* json);
Response* responseAllocJSONWithFormat(const char* format, ...);
Response* responseAlloc404NotFoundHTML(const char* resourcePathOrNull);
char* strdupDecodeGETParam(const char* paramNameIncludingEquals, const struct Request* request, const char* valueIfNotFound);

extern "C" {
struct Response* createResponseForRequest(const struct Request* request, struct Connection* connection);
}

namespace httpdebug {

    inline Server* httpServer = nullptr;
    inline std::thread* ewsThread = nullptr;
    inline std::atomic<bool> httpServerListening{ false };
    inline std::atomic<bool> serverReady{ false };
    inline std::atomic<bool> mainLoopStarted{ false };
    inline std::atomic<bool> shouldExit{ false };

    void startHttpServer(int port);
    void stopHttpServer();
    void signalReady();
    bool isReady();
    void waitForDebugCommand(const std::string& readyFile);
    void signalMainLoopStarted();
    void stopApp();

    inline std::atomic<bool> sdrStartRequest{ false };
    inline std::atomic<bool> sdrStopRequest{ false };
    inline std::atomic<bool> sdrPlaying{ false };
    inline std::string sourceChangeRequest{ "" };

    inline void requestSdrStart() {
        sdrStartRequest.store(true, std::memory_order_release);
    }
    inline void requestSdrStop() {
        sdrStopRequest.store(true, std::memory_order_release);
    }
    inline bool getSdrStartRequest() {
        return sdrStartRequest.exchange(false, std::memory_order_acq_rel);
    }
    inline bool getSdrStopRequest() {
        return sdrStopRequest.exchange(false, std::memory_order_acq_rel);
    }
    inline void setSdrPlaying(bool playing) {
        sdrPlaying.store(playing, std::memory_order_release);
    }
    inline bool isSdrPlaying() {
        return sdrPlaying.load(std::memory_order_acquire);
    }
    inline void requestSourceChange(const std::string& sourceName) {
        sourceChangeRequest = sourceName;
    }
    inline std::string getSourceChangeRequest() {
        std::string req = sourceChangeRequest;
        sourceChangeRequest = "";
        return req;
    }

#ifdef __cplusplus

    struct ImGuiAction {
        enum Type { Click,
                    MouseMove,
                    KeyPress,
                    TypeText,
                    Focus,
                    ClickById,
                    // Press at (x, y), move to (x2, y2) over `steps` frames, release.
                    // A click cannot stand in for this: everything that is dragged
                    // rather than clicked - the splitters, a VFO edge, a slider - only
                    // responds to the button being held down across frames, so without
                    // it none of those can be tested from outside.
                    Drag } type;
        float x, y;
        float x2, y2;
        int steps;
        int key;
        std::string text;
        ImGuiID targetId;
    };

    inline std::vector<ImGuiAction> pendingActions;
    inline std::mutex actionsMutex;

    void queueClick(float x, float y);
    void queueDrag(float x1, float y1, float x2, float y2, int steps);
    void queueKeyPress(int key);
    void queueTypeText(const std::string& text);
    void queueMouseMove(float x, float y);
    void queueFocus(ImGuiID id);
    void queueClickById(ImGuiID id);
    bool popAction(ImGuiAction& out);
    // Pops only if the next action is a pointer one, so the rest of the queue
    // stays in order for the pass that runs inside the frame.
    bool popPointerAction(ImGuiAction& out);
    std::string getAllWindowsJson();
    std::string getSimpleLayoutJson();

    struct WidgetInfo {
        ImGuiID id;
        std::string label;
        ImGuiItemStatusFlags flags;
        ImRect rect;
    };
    void registerWidget(ImGuiID id, ImGuiItemStatusFlags flags, const ImRect& rect);
    void clearWidgetRegistry();
    std::vector<WidgetInfo>& getWidgetRegistry();

#endif // __cplusplus

    // Module commands run on the UI thread, because most of them change what it is
    // drawing, and the request gives up after five seconds. A command that waits for
    // something - a decoder reaching a state - cannot live with either: it freezes
    // the window while it waits and is cut off before its own timeout.
    //
    // One registered here runs on the debug server's thread instead, under the
    // instance name and command it would otherwise have been sent to, and takes as
    // long as it takes. It must only touch state it guards itself, and must not
    // assume the module instance is still there.
    using OffThreadCommand = std::function<std::string(const std::string& args)>;
    void registerOffThreadCommand(const std::string& instanceName, const std::string& cmd, OffThreadCommand run);
    void unregisterOffThreadCommand(const std::string& instanceName, const std::string& cmd);
    // Empty when nothing is registered for that pair.
    OffThreadCommand findOffThreadCommand(const std::string& instanceName, const std::string& cmd);

    namespace procfs {
        struct ProcRequest {
            std::string path;
            std::string method;
            std::string body;
        };

        struct ProcResponse {
            int statusCode;
            std::string body;
            std::string contentType;
        };

        enum class Type { Unknown,
                          Bool,
                          Int,
                          Float,
                          String };

        using ReadFunc = std::function<std::string()>;
        using WriteFunc = std::function<void(const std::string&)>;

        int registerEndpoint(const std::string& path, ReadFunc read = nullptr, WriteFunc write = nullptr, Type type = Type::Unknown);
        void unregister(const std::string& path);

        std::vector<std::string> list();

        void processQueue();
        void queueRequest(const std::string& path, const std::string& method, const std::string& body, int responseId);
        bool getResponse(int responseId, ProcResponse& res);

    } // namespace procfs

} // namespace httpdebug