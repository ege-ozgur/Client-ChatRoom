#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <d3d11.h>
#include <tchar.h>
#include <thread>
#include <mutex>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <algorithm>
#include <cctype>
#include <iostream>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "GamesEngineeringBase.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "ws2_32.lib")

// Global variables for socket communication, synchronization, and application state
SOCKET g_socket = INVALID_SOCKET;
std::mutex g_dataMutex;
std::mutex g_soundMutex;

std::vector<std::string> g_globalChat;
std::vector<std::string> g_userList;
std::map<std::string, std::vector<std::string>> g_dmHistory;
std::set<std::string> g_openDMs;

// we track the login state and username in global variables for simplicity
bool g_loggedIn = false;
char g_usernameBuffer[64] = "";
char g_globalInputBuffer[256] = "";
std::string g_myUsername; // i made a login system for username selection and we store the username
// in a global variable for easy access across the application also we fill a buffer for the login input and a buffer for the global chat input

// this is the sound manager from the GamesEngineeringBase library we use it to play notification sounds for incoming messages and DMs
GamesEngineeringBase::SoundManager* g_audio = nullptr;

// Global variables for Direct3D 11 device and rendering
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
static bool g_SwapChainOccluded = false;
static UINT g_ResizeWidth = 0, g_ResizeHeight = 0;

// Forward declarations of helper functions for Direct3D setup and window procedure
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// i needed a helper function to trim whitespace from incoming messages to avoid processing empty or whitespace-only messages which could cause issues with the chat display and notification system
std::string trim(const std::string& str) {
    if (str.empty()) {
        return "";
    }
    size_t first = str.find_first_not_of(" \t\r\n\0");
    if (std::string::npos == first) {
        return "";
    }
    size_t last = str.find_last_not_of(" \t\r\n\0");
    return str.substr(first, (last - first + 1));
}

// this is the main loop that receives messages from the server asynchronously
// in a separate thread to avoid blocking the main UI thread
void ReceiveLoop() {
    // we initialize COM because the audio library uses XAudio2 internally
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    // we use a buffer to receive data from the socket
    char buffer[4096];
    std::string accumulatedData = "";

    // this loop runs until the server disconnects or an error occurs
    while (true) {
        // we clear the buffer before each receive to avoid leftover data
        memset(buffer, 0, sizeof(buffer));

        // we receive raw data from the socket
        int bytes = recv(g_socket, buffer, sizeof(buffer) - 1, 0);
        if (bytes <= 0) {
            // connection closed or error occurred
            break;
        }

        // we accumulate received data to handle split TCP packets
        accumulatedData += std::string(buffer, bytes);

        size_t pos = 0;
        // we process messages line by line using newline as a delimiter
        while ((pos = accumulatedData.find('\n')) != std::string::npos) {
            std::string incomingMsg = accumulatedData.substr(0, pos);
            accumulatedData.erase(0, pos + 1);

            // we trim whitespace and ignore empty messages
            incomingMsg = trim(incomingMsg);
            if (incomingMsg.empty()) {
                continue;
            }
            // we parse incoming messages based on a simple text protocol
            if (incomingMsg.rfind("USERS|", 0) == 0) {
                // we received an updated user list from the server
                std::lock_guard<std::mutex> lock(g_dataMutex);
                g_userList.clear();

                // we split the user list by commas and trim each username before adding it to the list
                std::string content = incomingMsg.substr(6);
                size_t p = 0;
                while ((p = content.find(',')) != std::string::npos) {
                    std::string u = content.substr(0, p);
                    if (!trim(u).empty())
                        g_userList.push_back(trim(u));
                    content.erase(0, p + 1);
                }
                // we add the last username after the final comma if it's not empty
                if (!trim(content).empty()) {
                    g_userList.push_back(trim(content));
                }     
            }
            else if (incomingMsg.rfind("DM|", 0) == 0) {
                // we handle private messages separately from the global chat
                size_t p1 = incomingMsg.find('|');
                size_t p2 = incomingMsg.find('|', p1 + 1);

                if (p2 != std::string::npos) {
                    std::string sender = trim(incomingMsg.substr(p1 + 1, p2 - (p1 + 1)));
                    std::string content = incomingMsg.substr(p2 + 1);
                    {
                        // we update private message history in a thread-safe manner
                        std::lock_guard<std::mutex> lock(g_dataMutex);
                        g_dmHistory[sender].push_back(sender + ": " + content);
                        g_openDMs.insert(sender);
                    }
                    // we play a dm notification sound only for messages sent by other users
                    if (_stricmp(sender.c_str(), g_myUsername.c_str()) != 0) {
                        std::lock_guard<std::mutex> soundLock(g_soundMutex);
                        if (g_audio)
                            g_audio->play("dm.wav");
                    }
                }
            }
            else if (incomingMsg.rfind("SYS|", 0) == 0) {
                // we treat system messages as informational and non-interactive
                // system messages do not trigger audio notifications
                std::string sysText = trim(incomingMsg.substr(4));
                if (!sysText.empty()) {
                    std::lock_guard<std::mutex> lock(g_dataMutex);
                    g_globalChat.push_back("[System] " + sysText);
                }
                continue;
            }
            else {
                // we process regular global chat messages
                std::string cleanMsg = trim(incomingMsg);
                if (cleanMsg.empty() || cleanMsg.size() <= 2) {
                    continue;
                }
                // we ignore messages sent by ourselves to avoid duplicate local feedback
                std::string myPrefix = g_myUsername + ":";
                if (cleanMsg.find(myPrefix) == 0) {
                    continue;
                }
                else {
                    // we update the global chat history in a thread-safe manner
                    std::lock_guard<std::mutex> lock(g_dataMutex);
                    g_globalChat.push_back(cleanMsg);
                }
                // we play a notification sound only for messages received from other users
                std::lock_guard<std::mutex> soundLock(g_soundMutex);
                if (g_audio) {
                    g_audio->play("message.wav");
                }                
            }
        }
    }

    // we uninitialize COM when the receive loop exits
    CoUninitialize();
}


int main(int, char**) {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    // we initialise the audio system and preload all sound assets at startup
    // the provided audio library manages its own internal source voices
    // so sounds are only triggered explicitly during playback
    g_audio = new GamesEngineeringBase::SoundManager();
    g_audio->load("message.wav"); // this is the sound that plays when a new message is received in the global chat
    g_audio->load("dm.wav"); // this is the sound that plays when a new direct message is received from another user
    g_audio->load("send.wav"); // this is the sound that plays when we send a message to provide local feedback

    // we set up the window class and create the application window using the Win32 API
    ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"ChatClient", nullptr };
    ::RegisterClassExW(&wc);

    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"Chat Client",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        100, 100, 1280, 800, nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd)) { // if Direct3D initialization fails, we clean up and exit the application
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    // we show the window and update it to trigger the initial paint message which will set up our Direct3D render target
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    // this is the main application loop that handles window messages, rendering, and user input
    bool done = false;
    while (!done) {
        // we process all pending window messages using PeekMessage to avoid blocking the main thread and allow for smooth rendering and input handling
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) { // we use PeekMessage in a loop to process all messages in the queue before rendering the next frame
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        // if the application is marked as done which means that when the user closes the window we close everything down and exit the main loop
        if (done) {
            break;
        }

        // if the window is minimized or occluded, we skip rendering to save resources and avoid unnecessary GPU work
        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            ::Sleep(10);
            continue;
        }
        g_SwapChainOccluded = false;

        // if the window was resized, we need to resize the Direct3D swap chain buffers and recreate the render target to match the new window size
        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        // we start a new ImGui frame for rendering the UI.
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // we get the ImGui IO object to access display size as I want make the login window and main chat window specifically positioned on the window
        ImGuiIO& io = ImGui::GetIO();

        if (!g_loggedIn) { // if the user is nopt logged in we start
            // we set the next window position to be centered on the screen and set a fixed size for the login window
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(300, 120));
            // we create the login window with no title bar, no resizing, no moving, and no collapsing to keep it simple and focused on the login process
            ImGui::Begin("Login", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);

            ImGui::Text("Enter Username:"); // we want the user to enter a username for the chat session
            ImGui::InputText("##username", g_usernameBuffer, IM_ARRAYSIZE(g_usernameBuffer)); // we use a hidden label for the input text to keep the UI clean and simple

            if (ImGui::Button("Connect", ImVec2(-1, 0))) { // when the user clicks the Connect button, we attempt to connect to the chat server using the provided username
                if (strlen(g_usernameBuffer) > 0) { // we check if the username is not empty before attempting to connect to avoid sending invalid data to the server
                    g_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                    sockaddr_in server{};
                    server.sin_family = AF_INET;
                    server.sin_port = htons(65432); // this is the servers port 
                    inet_pton(AF_INET, "127.0.0.1", &server.sin_addr); // this is the localhost

                    if (connect(g_socket, (sockaddr*)&server, sizeof(server)) != SOCKET_ERROR) { // if the connection is successful, we send the username to the server to join the chat and start the receive loop in a separate thread to listen for incoming messages
                        std::string joinMsg = std::string(g_usernameBuffer);
                        send(g_socket, joinMsg.c_str(), (int)joinMsg.size(), 0);

                        g_myUsername = trim(std::string(g_usernameBuffer));
                        g_loggedIn = true;
                        std::thread(ReceiveLoop).detach();
                    }
                }
            }
            ImGui::End(); // we end the login window and move on to rendering the main chat interface if the user is logged in
        }
        else { // if the user is logged in, we render the main chat interface which consists of a user list on the left and the global chat on the right with an input field at the bottom
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.35f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_Always); //we set the window size to be 800x600 and position it

            // we create the main chat window with no resizing, no moving, and no collapsing to keep it simple
            ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus;;
            ImGui::Begin("Chat Client - Main Chat Room", nullptr, window_flags);

            // we use ImGui columns to create a two-column layout for the user list and global chat, setting a fixed width for the user list column
            ImGui::Columns(2, "ChatColumns", true);
            ImGui::SetColumnWidth(0, 200);

            // we render the user list in the left column, allowing the user to select a username to open a direct message window with that user
            ImGui::Text("Users");
            ImGui::Separator(); {
                std::lock_guard<std::mutex> lock(g_dataMutex); // we lock the mutex to safely access the shared user list and render it in the UI
                for (const auto& user : g_userList) {
                    if (trim(user) == g_myUsername) { // we skip rendering our own username in the user list to avoid confusion and prevent opening a DM with ourselves
                        continue;
                    }
                    if (ImGui::Selectable(user.c_str())) {
                        g_openDMs.insert(user); 
                    }
                }
            }

            // we move to the next column to render the global chat interface
            ImGui::NextColumn();

            // we render the global chat messages in a scrollable child window, applying different text colors for messages sent by ourselves to provide visual feedback and distinguish them from messages sent by other users
            ImGui::Text("Global Chat");
            ImGui::Separator();
            ImGui::BeginChild("ScrollingRegion", ImVec2(0, -40), false, ImGuiWindowFlags_HorizontalScrollbar);
            {
                std::lock_guard<std::mutex> lock(g_dataMutex); // we lock the mutex to safely access the shared global chat history and render it in the UI
                for (const auto& msg : g_globalChat) { // we check if the message starts with our username followed by a colon to determine
                    //if it was sent by us and apply a different text color for our messages to provide visual feedback in the chat interface
                    std::string myPrefix = g_myUsername + ":";
                    if (msg.find(myPrefix) == 0 || msg.find(g_myUsername + " :") == 0) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
                        ImGui::TextWrapped("%s", msg.c_str());
                        ImGui::PopStyleColor();
                    }
                    else {
                        ImGui::TextWrapped("%s", msg.c_str());
                    }
                }
                if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) { // we check if the user has scrolled to the bottom of the chat and if so,
                    //we automatically scroll to the latest message when new messages arrive to keep the user updated with the most recent activity in the chat
                    ImGui::SetScrollHereY(1.0f);
                }
            }
            ImGui::EndChild();

            // we render the input field for sending messages to the global chat, allowing the user to type a message and send it by clicking the Send button or pressing Enter
            ImGui::Separator();
            ImGui::PushItemWidth(-60);
            ImGui::InputText("##GlobalInput", g_globalInputBuffer, IM_ARRAYSIZE(g_globalInputBuffer));
            ImGui::PopItemWidth();
            ImGui::SameLine();

            if (ImGui::Button("Send", ImVec2(50, 0))) { // when the user clicks the Send button, we check if the input buffer is not empty and then send the message to the server, adding a newline character as a message delimiter
                if (strlen(g_globalInputBuffer) > 0) {
                    std::string rawMsg = std::string(g_globalInputBuffer);
                    std::string msgToSend = rawMsg + "\n"; //
                    send(g_socket, msgToSend.c_str(), (int)msgToSend.size(), 0); {
                        std::lock_guard<std::mutex> lock(g_dataMutex);
                        g_globalChat.push_back(g_myUsername + ": " + rawMsg); 
                    }
                    // we play a send sound to provide local feedback when we send a message to the global chat, giving the user an audible confirmation that their message was sent successfully
                    std::lock_guard<std::mutex> soundLock(g_soundMutex);
                    if (g_audio) {
                        // we play the send sound for messages sent to the global chat
                        g_audio->play("send.wav");
                    }
                    g_globalInputBuffer[0] = '\0';
                }
            }
            ImGui::End();

            std::vector<std::string> dmsToClose;
            // we render open direct message windows for each user in the g_openDMs set, allowing the user to have multiple private conversations simultaneously and manage them through the UI
            for (const auto& targetUser : g_openDMs) {
                // we create a separate window for each open DM with a title indicating the target user
                bool open = true;
                // we make the window title dynamic based on the target user to provide context for the conversation and set a default size for the DM windows
                std::string windowTitle = "Private Chat with " + targetUser;
                // we create a smaller window for DMs and it's movable and resizable
                ImGui::SetNextWindowSize(ImVec2(400, 300), ImGuiCond_FirstUseEver);

                if (ImGui::Begin(windowTitle.c_str(), &open)) {
                    ImGui::BeginChild("DMMessages", ImVec2(0, -40));
                    {
                        // same message sending process for DMs
                        std::lock_guard<std::mutex> lock(g_dataMutex);
                        for (const auto& msg : g_dmHistory[targetUser]) {
                            // we check if the message starts with "Me:" to determine if it was sent by us and apply a different text color for our messages in the DM window to provide visual feedback and distinguish them from messages sent by the other user
                            if (msg.rfind("Me:", 0) == 0) {
                                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
                                ImGui::TextWrapped("%s", msg.c_str());
                                ImGui::PopStyleColor();
                            }
                            else { // this means the message was sent by the other user so we render it with the default text color to visually differentiate it from our messages in the DM conversation
                                ImGui::TextWrapped("%s", msg.c_str());
                            }
                        }
                        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
                            ImGui::SetScrollHereY(1.0f);
                        }
                    }
                    ImGui::EndChild();

                    // we render the input field for sending messages in the DM window, allowing the user to type a private message and send it to the target user by clicking the Send button or pressing Enter
                    ImGui::PushID(targetUser.c_str());
                    static char dmInput[256] = "";
                    ImGui::PushItemWidth(-60);
                    ImGui::InputText("##DMInput", dmInput, IM_ARRAYSIZE(dmInput));
                    ImGui::PopItemWidth();
                    ImGui::SameLine();

                    // when the user clicks the Send button in the DM window, we check if the input buffer is not empty and then send a DM message to the server in the format "DM|targetUser|message\n" to indicate that it's a direct message to the specified target user
                    if (ImGui::Button("Send")) {
                        if (strlen(dmInput) > 0) {
                            // we construct the DM message with the appropriate format and send it to the server, then we update the local DM history for the target user to include the sent message with a "Me:" prefix for visual feedback in the DM conversation
                            std::string packet = "DM|" + targetUser + "|" + std::string(dmInput) + "\n";
                            send(g_socket, packet.c_str(), (int)packet.size(), 0);
                            {
                                std::lock_guard<std::mutex> lock(g_dataMutex);
                                g_dmHistory[targetUser].push_back("Me: " + std::string(dmInput));
                            }
                            dmInput[0] = '\0';

                            std::lock_guard<std::mutex> soundLock(g_soundMutex);
                            // for DMs we play the same send sound
                            if (g_audio) {
                                g_audio->play("send.wav");
                            }

                        }
                    }
                    ImGui::PopID();
                }
                ImGui::End();
                // if the user closes the DM window by clicking the close button, we mark it for removal from the g_openDMs set to stop rendering it and free up resources associated with that DM conversation
                if (!open) {
                    dmsToClose.push_back(targetUser);
                }
            }

            for (const auto& user : dmsToClose) {
                // we remove the closed DM from the g_openDMs set to stop rendering it and clean up the DM history for that user if needed
                g_openDMs.erase(user);
            }
        }

        ImGui::Render();
        const float clear_color[4] = { 0.1f, 0.1f, 0.1f, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }

    // we clean up ImGui resources, Direct3D resources, and socket resources before exiting the application to ensure a graceful shutdown and free up system resources
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    closesocket(g_socket);
    WSACleanup();

    // we clean up the audio system by deleting the sound manager instance which will release all loaded sounds and XAudio2 resources, ensuring that we free up memory and properly shut down the audio subsystem when the application exits
    if (g_audio) {
        delete g_audio;
    }
    CoUninitialize();

    return 0;
}

// this function initializes the Direct3D 11 device, device context, and swap chain for rendering the application's graphics. It sets up the swap chain description with appropriate parameters for buffer count, format, usage, and windowed mode. If the initialization is successful, it creates the render target view for rendering to the back buffer. If any step fails, it returns false to indicate that Direct3D initialization was unsuccessful.
bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;

    if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, nullptr, &g_pd3dDeviceContext) != S_OK)
        return false;
    CreateRenderTarget();
    return true;
}

// this function releases all Direct3D resources including the render target view, swap chain, device context, and device to ensure that we free up GPU resources and avoid memory leaks when the application exits or when Direct3D needs to be reinitialized due to a window resize or other events that require resetting the graphics device.
void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain) g_pSwapChain->Release();
    if (g_pd3dDeviceContext) g_pd3dDeviceContext->Release();
    if (g_pd3dDevice) g_pd3dDevice->Release();
}
// this function creates a render target view for the back buffer of the swap chain, allowing us to render our graphics to the window. It retrieves the back buffer texture from the swap chain and creates a render target view using the Direct3D device. This render target view is then used in the rendering loop to set the output target for drawing operations. After creating the render target view, it releases the reference to the back buffer texture to free up resources.
void CreateRenderTarget()
{
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

// this function releases the render target view resource to free up GPU resources when it's no longer needed, such as when the application is closing or when the window is resized and the render target needs to be recreated to match the new window size. It checks if the render target view exists and releases it if it does, ensuring that we properly clean up Direct3D resources to avoid memory leaks and ensure a graceful shutdown of the graphics subsystem.
void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) g_mainRenderTargetView->Release();
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg)
    {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) return 0;
        g_ResizeWidth = (UINT)LOWORD(lParam);
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
