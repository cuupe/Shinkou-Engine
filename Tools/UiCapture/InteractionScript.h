#pragma once

// Included in WindowCapture.cpp's anonymous namespace. Every action travels
// through Win32 -> SDL -> InputSystem -> EditorUi; no editor command injection.
bool run_interaction_script(HWND window, const std::filesystem::path& script,
                            const std::filesystem::path& output, const std::filesystem::path& snapshot) {
    std::ifstream input(script);
    std::ofstream log(std::filesystem::path(output).concat(".steps.jsonl"));
    if (!input || !log) return false;
    auto pause = [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(std::clamp(ms,0,30000))); };
    // Targeted Win32 messages also work in an unattended/non-foreground
    // desktop. They exercise the native window/SDL input route without typing
    // into another application. Record this mode separately from SendInput.
    PostMessageW(window,WM_SETFOCUS,0,0);
    struct CursorRestore { POINT position{}; CursorRestore() { GetCursorPos(&position); } ~CursorRestore() { SetCursorPos(position.x,position.y); } } restoreCursor;
    POINT cursor{};
    auto key = [&](WORD vk, bool up = false, bool unicode = false) {
        if(unicode) { if(!up) PostMessageW(window,WM_CHAR,vk,1); return; }
        const auto scan=MapVirtualKeyW(vk,MAPVK_VK_TO_VSC);
        LPARAM flags=1 | (static_cast<LPARAM>(scan)<<16);
        if(vk==VK_HOME || vk==VK_END || vk==VK_PRIOR || vk==VK_NEXT ||
           vk==VK_LEFT || vk==VK_RIGHT || vk==VK_UP || vk==VK_DOWN ||
           vk==VK_INSERT || vk==VK_DELETE || vk==VK_RCONTROL || vk==VK_RMENU)
            flags |= static_cast<LPARAM>(1)<<24;
        if(up) flags |= (static_cast<LPARAM>(1)<<30)|(static_cast<LPARAM>(1)<<31);
        PostMessageW(window,up ? WM_KEYUP : WM_KEYDOWN,vk,flags);
    };
    std::string lastState, lastCamera, rememberedCamera;
    auto region = [&](const std::string& id) {
        DeleteFileW(snapshot.wstring().c_str());
        PostMessageW(window,WM_COMMAND,49000,0);
        std::string snapshotText;
        bool complete = false;
        for(int i=0;i<150;++i) {
            pause(20); std::ifstream file(snapshot);
            snapshotText.assign(std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>());
            if (snapshotText.find("\nend\n")!=std::string::npos) { complete=true; break; }
        }
        if(!complete) throw std::runtime_error("State snapshot timed out");
        std::istringstream state(snapshotText);
        std::string line;
        float scale=1;
        RECT result{}; bool found=false;
        while (std::getline(state,line)) {
            std::istringstream row(line); std::string kind; row>>kind;
            if (kind=="state") {row>>scale; lastState=line;}
            else if(kind=="camera") lastCamera=line;
            else if(kind=="region") {
                std::string name; float x{},y{},w{},h{}; row>>std::quoted(name)>>x>>y>>w>>h;
                if(name==id && w>0 && h>0) { result={static_cast<LONG>(x*scale),static_cast<LONG>(y*scale),static_cast<LONG>((x+w)*scale),static_cast<LONG>((y+h)*scale)};found=true; }
            }
        }
        if (!found) throw std::runtime_error("No visible region: "+id);
        return result;
    };
    WPARAM mouseButtons = 0;
    auto pointer = [&](LONG x, LONG y) {
        cursor={x,y}; POINT screen=cursor; ClientToScreen(window,&screen);
        // SDL samples the global cursor between pumps, including while dragging.
        // Keep that sample consistent with the targeted native motion payload.
        SetCursorPos(screen.x,screen.y);
        PostMessageW(window,WM_MOUSEMOVE,mouseButtons,MAKELPARAM(x,y));pause(35);
    };
    auto mouse = [&](DWORD flags, DWORD data=0) {
        UINT message=flags==MOUSEEVENTF_LEFTDOWN ? WM_LBUTTONDOWN : flags==MOUSEEVENTF_LEFTUP ? WM_LBUTTONUP : flags==MOUSEEVENTF_RIGHTDOWN ? WM_RBUTTONDOWN : flags==MOUSEEVENTF_RIGHTUP ? WM_RBUTTONUP : flags==MOUSEEVENTF_MIDDLEDOWN ? WM_MBUTTONDOWN : flags==MOUSEEVENTF_MIDDLEUP ? WM_MBUTTONUP : WM_MOUSEWHEEL;
        POINT point=cursor;if(message==WM_MOUSEWHEEL) ClientToScreen(window,&point);
        // SDL derives button coordinates from its last motion. Queue both in
        // the same pump so an idle global-cursor sample cannot separate them.
        PostMessageW(window,WM_MOUSEMOVE,mouseButtons,MAKELPARAM(cursor.x,cursor.y));
        if(message==WM_LBUTTONDOWN) mouseButtons |= MK_LBUTTON;
        if(message==WM_RBUTTONDOWN) mouseButtons |= MK_RBUTTON;
        if(message==WM_LBUTTONUP) mouseButtons &= ~MK_LBUTTON;
        if(message==WM_RBUTTONUP) mouseButtons &= ~MK_RBUTTON;
        if(message==WM_MBUTTONDOWN) mouseButtons |= MK_MBUTTON;
        if(message==WM_MBUTTONUP) mouseButtons &= ~MK_MBUTTON;
        PostMessageW(window,message,message==WM_MOUSEWHEEL ? MAKEWPARAM(mouseButtons,static_cast<SHORT>(data)) : mouseButtons,MAKELPARAM(point.x,point.y));
    };
    auto capture = [&](std::string name) {
        DeleteFileW(output.wstring().c_str());PostMessageW(window,WM_COMMAND,49001,0);
        bool ready=false;
        for(int i=0;i<150;++i) { pause(20);int w{},h{};if(read_bitmap_dimensions(output,w,h)){ready=true;break;} }
        if(!ready) throw std::runtime_error("GPU capture timed out");
        if(name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")!=std::string::npos) throw std::runtime_error("Invalid capture name");
        std::filesystem::copy_file(output,output.parent_path()/(name+".bmp"),std::filesystem::copy_options::overwrite_existing);
        std::filesystem::copy_file(snapshot,output.parent_path()/(name+".state.txt"),std::filesystem::copy_options::overwrite_existing);
    };
    log << "{\"mode\":\"TargetedWin32Messages\"}\n";
    std::string line;std::size_t step=0;
    try {
        while(std::getline(input,line)) {
            if(line.empty()||line[0]=='#') continue;
            ++step;
            std::istringstream row(line);std::string op;row>>op;
            if(op=="wait") {int ms=200;row>>ms;pause(ms);}
            else if(op=="click"||op=="double"||op=="right"||op=="hover"||op=="sweep") {
                std::string id;row>>std::quoted(id);const auto r=region(id);
                pointer((r.left+r.right)/2,(r.top+r.bottom)/2);
                if(op=="sweep") {int count=60;row>>count;for(int i=0;i<std::clamp(count,1,600);++i) pointer(r.left+2+(r.right-r.left-4)*(i%20)/20,(r.top+r.bottom)/2);}
                else if(op!="hover") for(int i=0;i<(op=="double"?2:1);++i) {mouse(op=="right"?MOUSEEVENTF_RIGHTDOWN:MOUSEEVENTF_LEFTDOWN);pause(35);mouse(op=="right"?MOUSEEVENTF_RIGHTUP:MOUSEEVENTF_LEFTUP);pause(60);}
            } else if(op=="orbit") {
                const auto r=region("viewport.surface");
                pointer((r.left+r.right)/2,(r.top+r.bottom)/2);mouse(MOUSEEVENTF_MIDDLEDOWN);pause(50);
                for(int i=1;i<=20;++i) pointer((r.left+r.right)/2+i*3,(r.top+r.bottom)/2+i);
                mouse(MOUSEEVENTF_MIDDLEUP);
            } else if(op=="drag") {
                std::string id;float target=0;row>>std::quoted(id)>>target;const auto r=region(id);
                pointer((r.left+r.right)/2,(r.top+r.bottom)/2);mouse(MOUSEEVENTF_LEFTDOWN);pause(50);
                for(int i=0;i<20;++i) pointer((r.left+r.right)/2,static_cast<LONG>((r.top+r.bottom)*0.5f+(r.top+(r.bottom-r.top)*target-(r.top+r.bottom)*0.5f)*(i+1)/20));
                mouse(MOUSEEVENTF_LEFTUP);
            } else if(op=="key"||op=="down"||op=="up") {
                unsigned vk{};row>>vk;if(vk>255) throw std::runtime_error("Invalid virtual key");key(static_cast<WORD>(vk),op=="up");if(op=="key"){pause(35);key(static_cast<WORD>(vk),true);}
            } else if(op=="text") {
                std::string text;row>>std::quoted(text);
                const auto count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
                std::wstring wide(count,L'\0');MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),wide.data(),count);
                for(wchar_t c:wide){key(c,false,true);key(c,true,true);pause(40);}
            } else if(op=="wheel") {int delta=0;row>>delta;mouse(MOUSEEVENTF_WHEEL,static_cast<DWORD>(delta*WHEEL_DELTA));}
            else if(op=="resize") {std::wstring size;std::string token;row>>token;size.assign(token.begin(),token.end());Size value{};if(!parse_size(size,value)||!resize_client(window,value))throw std::runtime_error("Resize failed");}
            else if(op=="capture") {std::string name;row>>name;region("command:play");capture(name);}
            else if(op=="exists"||op=="missing") {std::string path;row>>std::quoted(path);const bool exists=std::filesystem::exists(std::filesystem::u8path(path));if(exists!=(op=="exists"))throw std::runtime_error("Filesystem assertion failed: "+path);}
            else if(op=="state") {std::string expected;row>>std::quoted(expected);region("command:play");if(lastState.find(expected)==std::string::npos)throw std::runtime_error("State assertion failed: "+expected+"; "+lastState);}
            else if(op=="remember-camera") {region("command:play");rememberedCamera=lastCamera;if(rememberedCamera.empty())throw std::runtime_error("No active camera");}
            else if(op=="camera-changed") {region("command:play");if(lastCamera==rememberedCamera || lastCamera.empty())throw std::runtime_error("Camera did not change");}
            else if(op=="scroll-min" || op=="scroll-max") {float bound=0;row>>bound;region("command:play");const auto i=lastState.find(" scroll=");if(i==std::string::npos)throw std::runtime_error("No scroll state");const auto value=std::stof(lastState.substr(i+8));if(op=="scroll-min" ? value<bound : value>bound)throw std::runtime_error("Scroll assertion failed: "+std::to_string(value));}
            else throw std::runtime_error("Unknown script command: "+op);
            pause(120);
            log << "{\"step\":"<<step<<",\"action\":"<<std::quoted(line)<<",\"ok\":true}\n";
        }
        return true;
    } catch(const std::exception& e) {
        log<<"{\"step\":"<<step<<",\"ok\":false,\"error\":"<<std::quoted(e.what())<<"}\n";
        std::cerr<<"Interaction step "<<step<<" failed: "<<e.what()<<'\n';return false;
    }
}
