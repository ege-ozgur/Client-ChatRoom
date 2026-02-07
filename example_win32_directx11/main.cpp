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

SOCKET g_socket = INVALID_SOCKET;
std::mutex g_dataMutex;
std::mutex g_soundMutex;

std::vector<std::string> g_globalChat;
std::vector<std::string> g_userList;
std::map<std::string, std::vector<std::string>> g_dmHistory;
std::set<std::string> g_openDMs;

bool g_loggedIn = false;
char g_usernameBuffer[64] = "";
char g_globalInputBuffer[256] = "";
std::string g_myUsername;

GamesEngineeringBase::SoundManager* g_audio = nullptr;

static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
static bool g_SwapChainOccluded = false;
static UINT g_ResizeWidth = 0, g_ResizeHeight = 0;

bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

std::string trim(const std::string& str)
{
    if (str.empty()) return "";
    size_t first = str.find_first_not_of(" \t\r\n\0");
    if (std::string::npos == first) return "";
    size_t last = str.find_last_not_of(" \t\r\n\0");
    return str.substr(first, (last - first + 1));
}

void ReceiveLoop()
{
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    char buffer[4096];
    std::string accumulatedData = "";

    while (true)
    {
        memset(buffer, 0, sizeof(buffer));
        int bytes = recv(g_socket, buffer, sizeof(buffer) - 1, 0);
        if (bytes <= 0) break;

        accumulatedData += std::string(buffer, bytes);

        size_t pos = 0;
        while ((pos = accumulatedData.find('\n')) != std::string::npos)
        {
            std::string incomingMsg = accumulatedData.substr(0, pos);
            accumulatedData.erase(0, pos + 1);

            incomingMsg = trim(incomingMsg);
            if (incomingMsg.empty()) continue;

            if (incomingMsg.rfind("USERS|", 0) == 0)
            {
                std::lock_guard<std::mutex> lock(g_dataMutex);
                g_userList.clear();
                std::string content = incomingMsg.substr(6);
                size_t p = 0;
                while ((p = content.find(',')) != std::string::npos)
                {
                    std::string u = content.substr(0, p);
                    if (!trim(u).empty()) g_userList.push_back(trim(u));
                    content.erase(0, p + 1);
                }
                if (!trim(content).empty()) g_userList.push_back(trim(content));
            }
            else if (incomingMsg.rfind("DM|", 0) == 0)
            {
                size_t p1 = incomingMsg.find('|');
                size_t p2 = incomingMsg.find('|', p1 + 1);

                if (p2 != std::string::npos)
                {
                    std::string sender = trim(incomingMsg.substr(p1 + 1, p2 - (p1 + 1)));
                    std::string content = incomingMsg.substr(p2 + 1);

                    {
                        std::lock_guard<std::mutex> lock(g_dataMutex);
                        g_dmHistory[sender].push_back(sender + ": " + content);
                        g_openDMs.insert(sender);
                    }

                    if (_stricmp(sender.c_str(), g_myUsername.c_str()) != 0)
                    {
                        std::lock_guard<std::mutex> soundLock(g_soundMutex);
                        if (g_audio) g_audio->play("dm.wav");
                    }
                }
            }
            else if (incomingMsg.rfind("SYS|", 0) == 0)
            {
                std::string sysText = trim(incomingMsg.substr(4));
                if (!sysText.empty())
                {
                    std::lock_guard<std::mutex> lock(g_dataMutex);
                    g_globalChat.push_back("[System] " + sysText);
                }
                continue;
            }
            else
            {
                std::string cleanMsg = trim(incomingMsg);
                if (cleanMsg.empty() || cleanMsg.size() <= 2) continue;
                std::string myPrefix = g_myUsername + ":";
                if (cleanMsg.find(myPrefix) == 0)
                {
                    continue;
                }

                {
                    std::lock_guard<std::mutex> lock(g_dataMutex);
                    g_globalChat.push_back(cleanMsg);
                }
                std::lock_guard<std::mutex> soundLock(g_soundMutex);
                if (g_audio) g_audio->play("message.wav");
            }
        }
    }
    CoUninitialize();
}

int main(int, char**)
{
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    g_audio = new GamesEngineeringBase::SoundManager();
    g_audio->load("message.wav");
    g_audio->load("dm.wav");
    g_audio->load("send.wav");

    ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"ChatClient", nullptr };
    ::RegisterClassExW(&wc);

    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"Chat Client",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        100, 100, 1280, 800, nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    bool done = false;
    while (!done)
    {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
        {
            ::Sleep(10);
            continue;
        }
        g_SwapChainOccluded = false;

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0)
        {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGuiIO& io = ImGui::GetIO();

        if (!g_loggedIn)
        {
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(300, 120));
            ImGui::Begin("Login", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);

            ImGui::Text("Enter Username:");
            ImGui::InputText("##username", g_usernameBuffer, IM_ARRAYSIZE(g_usernameBuffer));

            if (ImGui::Button("Connect", ImVec2(-1, 0)))
            {
                if (strlen(g_usernameBuffer) > 0)
                {
                    g_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                    sockaddr_in server{};
                    server.sin_family = AF_INET;
                    server.sin_port = htons(65432);
                    inet_pton(AF_INET, "127.0.0.1", &server.sin_addr);

                    if (connect(g_socket, (sockaddr*)&server, sizeof(server)) != SOCKET_ERROR)
                    {
                        std::string joinMsg = std::string(g_usernameBuffer);
                        send(g_socket, joinMsg.c_str(), (int)joinMsg.size(), 0);

                        g_myUsername = trim(std::string(g_usernameBuffer));
                        g_loggedIn = true;
                        std::thread(ReceiveLoop).detach();
                    }
                }
            }
            ImGui::End();
        }
        else
        {
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.35f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_Always);

            ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse;
            ImGui::Begin("Chat Client - Main Chat Room", nullptr, window_flags);

            ImGui::Columns(2, "ChatColumns", true);
            ImGui::SetColumnWidth(0, 200);

            ImGui::Text("Users");
            ImGui::Separator();
            {
                std::lock_guard<std::mutex> lock(g_dataMutex);
                for (const auto& user : g_userList)
                {
                    if (trim(user) == g_myUsername) continue;
                    if (ImGui::Selectable(user.c_str()))
                    {
                        g_openDMs.insert(user);
                    }
                }
            }

            ImGui::NextColumn();

            ImGui::Text("Global Chat");
            ImGui::Separator();
            ImGui::BeginChild("ScrollingRegion", ImVec2(0, -40), false, ImGuiWindowFlags_HorizontalScrollbar);
            {
                std::lock_guard<std::mutex> lock(g_dataMutex);
                for (const auto& msg : g_globalChat)
                {
                    std::string myPrefix = g_myUsername + ":";
                    if (msg.find(myPrefix) == 0 || msg.find(g_myUsername + " :") == 0)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
                        ImGui::TextWrapped("%s", msg.c_str());
                        ImGui::PopStyleColor();
                    }
                    else
                    {
                        ImGui::TextWrapped("%s", msg.c_str());
                    }
                }
                if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                    ImGui::SetScrollHereY(1.0f);
            }
            ImGui::EndChild();

            ImGui::Separator();
            ImGui::PushItemWidth(-60);
            ImGui::InputText("##GlobalInput", g_globalInputBuffer, IM_ARRAYSIZE(g_globalInputBuffer));
            ImGui::PopItemWidth();
            ImGui::SameLine();

            if (ImGui::Button("Send", ImVec2(50, 0)))
            {
                if (strlen(g_globalInputBuffer) > 0)
                {
                    std::string rawMsg = std::string(g_globalInputBuffer);
                    std::string msgToSend = rawMsg + "\n";
                    send(g_socket, msgToSend.c_str(), (int)msgToSend.size(), 0);

                    {
                        std::lock_guard<std::mutex> lock(g_dataMutex);
                        g_globalChat.push_back(g_myUsername + ": " + rawMsg);
                    }
                    std::lock_guard<std::mutex> soundLock(g_soundMutex);
                    if (g_audio) g_audio->play("send.wav");
                    g_globalInputBuffer[0] = '\0';
                }
            }
            ImGui::End();

            std::vector<std::string> dmsToClose;
            for (const auto& targetUser : g_openDMs)
            {
                bool open = true;
                std::string windowTitle = "Private Chat with " + targetUser;
                ImGui::SetNextWindowSize(ImVec2(400, 300), ImGuiCond_FirstUseEver);

                if (ImGui::Begin(windowTitle.c_str(), &open))
                {
                    ImGui::BeginChild("DMMessages", ImVec2(0, -40));
                    {
                        std::lock_guard<std::mutex> lock(g_dataMutex);
                        for (const auto& msg : g_dmHistory[targetUser])
                        {
                            if (msg.rfind("Me:", 0) == 0)
                            {
                                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
                                ImGui::TextWrapped("%s", msg.c_str());
                                ImGui::PopStyleColor();
                            }
                            else
                            {
                                ImGui::TextWrapped("%s", msg.c_str());
                            }
                        }
                        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                            ImGui::SetScrollHereY(1.0f);
                    }
                    ImGui::EndChild();

                    ImGui::PushID(targetUser.c_str());
                    static char dmInput[256] = "";
                    ImGui::PushItemWidth(-60);
                    ImGui::InputText("##DMInput", dmInput, IM_ARRAYSIZE(dmInput));
                    ImGui::PopItemWidth();
                    ImGui::SameLine();

                    if (ImGui::Button("Send"))
                    {
                        if (strlen(dmInput) > 0)
                        {
                            std::string packet = "DM|" + targetUser + "|" + std::string(dmInput) + "\n";
                            send(g_socket, packet.c_str(), (int)packet.size(), 0);

                            {
                                std::lock_guard<std::mutex> lock(g_dataMutex);
                                g_dmHistory[targetUser].push_back("Me: " + std::string(dmInput));
                            }
                            dmInput[0] = '\0';

                            std::lock_guard<std::mutex> soundLock(g_soundMutex);
                            if (g_audio) g_audio->play("send.wav");

                        }
                    }
                    ImGui::PopID();
                }
                ImGui::End();

                if (!open) dmsToClose.push_back(targetUser);
            }

            for (const auto& user : dmsToClose) {
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

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    closesocket(g_socket);
    WSACleanup();

    if (g_audio) delete g_audio;
    CoUninitialize();

    return 0;
}

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

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain) g_pSwapChain->Release();
    if (g_pd3dDeviceContext) g_pd3dDeviceContext->Release();
    if (g_pd3dDevice) g_pd3dDevice->Release();
}

void CreateRenderTarget()
{
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

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
