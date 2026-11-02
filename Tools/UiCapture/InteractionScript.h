#pragma once

// Included in WindowCapture.cpp's anonymous namespace. Every action travels
// through Win32 -> SDL -> InputSystem -> EditorUi; no editor command injection.
bool run_interaction_script(HWND window, const std::filesystem::path& script,
                            const std::filesystem::path& output, const std::filesystem::path& snapshot,
                            bool nativeKeyboard = false, bool queuedKeyboard = false) {
    std::ifstream input(script);
    std::ofstream log(std::filesystem::path(output).concat(".steps.jsonl"));
    if (!input || !log) return false;
    auto pause = [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(std::clamp(ms,0,30000))); };
    // Targeted Win32 messages also work in an unattended/non-foreground
    // desktop. They exercise the native window/SDL input route without typing
    // into another application. Record this mode separately from SendInput.
    PostMessageW(window,WM_SETFOCUS,0,0);
    SetForegroundWindow(window);
    SetActiveWindow(window);
    SetFocus(window);
    struct CursorRestore { POINT position{}; CursorRestore() { GetCursorPos(&position); } ~CursorRestore() { SetCursorPos(position.x,position.y); } } restoreCursor;
    POINT cursor{};
    HWND activeWindow=window;
    struct KeyboardQueue {
        DWORD editorThread{0};
        std::vector<BYTE> state=std::vector<BYTE>(256,0);
        ~KeyboardQueue() {
            if (!editorThread) return;
            std::fill(state.begin(),state.end(),0);
            SetKeyboardState(state.data());
            AttachThreadInput(GetCurrentThreadId(),editorThread,FALSE);
        }
    } queue;
    if (queuedKeyboard) {
        const auto editorThread=GetWindowThreadProcessId(window,nullptr);
        if (!AttachThreadInput(GetCurrentThreadId(),editorThread,TRUE)) {
            std::cerr<<"Could not attach to the editor's keyboard queue\n";
            return false;
        }
        queue.editorThread=editorThread;
    }
    struct HeldKeys {
        std::vector<WORD> keys;
        ~HeldKeys() { for (const auto key : keys) { INPUT event{}; event.type=INPUT_KEYBOARD; event.ki.wVk=key; event.ki.dwFlags=KEYEVENTF_KEYUP; SendInput(1,&event,sizeof(event)); } }
    } held;
    auto key = [&](WORD vk, bool up = false, bool unicode = false) {
        if (queuedKeyboard && !unicode) {
            queue.state[vk]=up?0:0x80;
            if (vk==VK_CONTROL) queue.state[VK_LCONTROL]=queue.state[vk];
            if (vk==VK_SHIFT) queue.state[VK_LSHIFT]=queue.state[vk];
            if (vk==VK_MENU) queue.state[VK_LMENU]=queue.state[vk];
            if (!SetKeyboardState(queue.state.data())) throw std::runtime_error("Editor keyboard queue state was rejected");
        }
        if (nativeKeyboard && !queuedKeyboard) {
            // Win32's ImGui backend reads OS modifier state. Posted WM_KEYDOWN
            // messages cannot simulate Ctrl/Shift state; use native keyboard
            // injection only while our own editor window has foreground focus.
            if (GetForegroundWindow()!=window) { SetForegroundWindow(window); pause(60); }
            if (GetForegroundWindow()!=window) throw std::runtime_error("Editor lost foreground focus before native keyboard input");
            INPUT event{}; event.type=INPUT_KEYBOARD;
            event.ki.wVk=unicode?0:vk; event.ki.wScan=unicode?vk:0;
            event.ki.dwFlags=(unicode?KEYEVENTF_UNICODE:0)|(up?KEYEVENTF_KEYUP:0);
            if (SendInput(1,&event,sizeof(event))!=1) throw std::runtime_error("Native keyboard input was rejected");
            if (unicode && !up) { event.ki.dwFlags|=KEYEVENTF_KEYUP; SendInput(1,&event,sizeof(event)); }
            else if (!unicode) {
                if (up) held.keys.erase(std::remove(held.keys.begin(),held.keys.end(),vk),held.keys.end());
                else if (std::find(held.keys.begin(),held.keys.end(),vk)==held.keys.end()) held.keys.push_back(vk);
            }
            return;
        }
        if(unicode) { if(!up) PostMessageW(activeWindow,WM_CHAR,vk,1); return; }
        const auto scan=MapVirtualKeyW(vk,MAPVK_VK_TO_VSC);
        LPARAM flags=1 | (static_cast<LPARAM>(scan)<<16);
        if(vk==VK_HOME || vk==VK_END || vk==VK_PRIOR || vk==VK_NEXT ||
           vk==VK_LEFT || vk==VK_RIGHT || vk==VK_UP || vk==VK_DOWN ||
           vk==VK_INSERT || vk==VK_DELETE || vk==VK_RCONTROL || vk==VK_RMENU)
            flags |= static_cast<LPARAM>(1)<<24;
        if(up) flags |= (static_cast<LPARAM>(1)<<30)|(static_cast<LPARAM>(1)<<31);
        PostMessageW(activeWindow,up ? WM_KEYUP : WM_KEYDOWN,vk,flags);
    };
    std::string lastState, lastCamera, rememberedCamera, lastObject;
    std::unordered_map<std::string,std::string> rememberedProperties;
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
        lastObject.clear();
        std::string line;
        float scale=1;
        RECT result{}; bool found=false;activeWindow=window;
        RECT surfaceRect{};HWND surfaceWindow=nullptr;
        while (std::getline(state,line)) {
            std::istringstream row(line); std::string kind; row>>kind;
            if (kind=="state") {row>>scale; lastState=line;}
            else if(kind=="camera") lastCamera=line;
            else if(kind=="object") row>>std::quoted(lastObject);
            else if(kind=="region") {
                std::string name; float x{},y{},w{},h{}; row>>std::quoted(name)>>x>>y>>w>>h;
                if(name==id && w>0 && h>0) { result={static_cast<LONG>(x*scale),static_cast<LONG>(y*scale),static_cast<LONG>((x+w)*scale),static_cast<LONG>((y+h)*scale)};found=true; }
            }
            else if(kind=="surface") {
                std::string name;std::uintptr_t handle{};float x{},y{},w{},h{};row>>std::quoted(name)>>handle>>x>>y>>w>>h;
                if(name==id&&handle&&w>0&&h>0){surfaceWindow=reinterpret_cast<HWND>(handle);surfaceRect={LONG(x),LONG(y),LONG(x+w),LONG(y+h)};}
            }
        }
        if (!found) throw std::runtime_error("No visible region: "+id);
        if(surfaceWindow){activeWindow=surfaceWindow;result=surfaceRect;}
        return result;
    };
    WPARAM mouseButtons = 0;
    auto pointer = [&](LONG x, LONG y) {
        if(GetForegroundWindow()!=activeWindow || GetFocus()!=activeWindow) {
            SetForegroundWindow(activeWindow);SetActiveWindow(activeWindow);SetFocus(activeWindow);pause(150);
        }
        cursor={x,y}; POINT screen=cursor; ClientToScreen(activeWindow,&screen);
        // SDL samples the global cursor between pumps, including while dragging.
        // Keep that sample consistent with the targeted native motion payload.
        SetCursorPos(screen.x,screen.y);
        POINT observed{};GetCursorPos(&observed);RECT bounds{};GetWindowRect(activeWindow,&bounds);
        log<<"{\"pointer\":["<<screen.x<<','<<screen.y<<"],\"observed\":["<<observed.x<<','<<observed.y<<"],\"window\":"<<reinterpret_cast<std::uintptr_t>(activeWindow)
            <<",\"hit\":"<<reinterpret_cast<std::uintptr_t>(WindowFromPoint(screen))<<",\"bounds\":["<<bounds.left<<','<<bounds.top<<','<<bounds.right<<','<<bounds.bottom<<"]}\n";
        PostMessageW(activeWindow,WM_MOUSEMOVE,mouseButtons,MAKELPARAM(x,y));pause(35);
    };
    auto mouse = [&](DWORD flags, DWORD data=0) {
        if(flags==MOUSEEVENTF_LEFTDOWN||flags==MOUSEEVENTF_RIGHTDOWN||flags==MOUSEEVENTF_MIDDLEDOWN) {
            // Real Windows clicks activate the destination before delivery.
            // Targeted PostMessage input must reproduce that OS focus step.
            if(GetForegroundWindow()!=activeWindow)SetForegroundWindow(activeWindow);
            SetActiveWindow(activeWindow);SetFocus(activeWindow);
        }
        UINT message=flags==MOUSEEVENTF_LEFTDOWN ? WM_LBUTTONDOWN : flags==MOUSEEVENTF_LEFTUP ? WM_LBUTTONUP : flags==MOUSEEVENTF_RIGHTDOWN ? WM_RBUTTONDOWN : flags==MOUSEEVENTF_RIGHTUP ? WM_RBUTTONUP : flags==MOUSEEVENTF_MIDDLEDOWN ? WM_MBUTTONDOWN : flags==MOUSEEVENTF_MIDDLEUP ? WM_MBUTTONUP : WM_MOUSEWHEEL;
        POINT point=cursor;if(message==WM_MOUSEWHEEL) ClientToScreen(activeWindow,&point);
        // SDL derives button coordinates from its last motion. Queue both in
        // the same pump so an idle global-cursor sample cannot separate them.
        PostMessageW(activeWindow,WM_MOUSEMOVE,mouseButtons,MAKELPARAM(cursor.x,cursor.y));
        if(message==WM_LBUTTONDOWN) mouseButtons |= MK_LBUTTON;
        if(message==WM_RBUTTONDOWN) mouseButtons |= MK_RBUTTON;
        if(message==WM_LBUTTONUP) mouseButtons &= ~MK_LBUTTON;
        if(message==WM_RBUTTONUP) mouseButtons &= ~MK_RBUTTON;
        if(message==WM_MBUTTONDOWN) mouseButtons |= MK_MBUTTON;
        if(message==WM_MBUTTONUP) mouseButtons &= ~MK_MBUTTON;
        PostMessageW(activeWindow,message,message==WM_MOUSEWHEEL ? MAKEWPARAM(mouseButtons,static_cast<SHORT>(data)) : mouseButtons,MAKELPARAM(point.x,point.y));
    };
    auto capture = [&](std::string name) {
        {std::ofstream target(std::filesystem::path(output).concat(".target"));target<<(activeWindow==window?0:reinterpret_cast<std::uintptr_t>(activeWindow));}
        DeleteFileW(output.wstring().c_str());PostMessageW(window,WM_COMMAND,49001,0);
        bool ready=false;
        for(int i=0;i<150;++i) { pause(20);int w{},h{};if(read_bitmap_dimensions(output,w,h)){ready=true;break;} }
        if(!ready) throw std::runtime_error("GPU capture timed out");
        if(name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")!=std::string::npos) throw std::runtime_error("Invalid capture name");
        std::filesystem::copy_file(output,output.parent_path()/(name+".bmp"),std::filesystem::copy_options::overwrite_existing);
        std::filesystem::copy_file(snapshot,output.parent_path()/(name+".state.txt"),std::filesystem::copy_options::overwrite_existing);
    };
    log << "{\"mode\":\"" << (queuedKeyboard?"AttachedEditorKeyboardQueue":nativeKeyboard?"NativeKeyboardAndTargetedWin32Mouse":"TargetedWin32Messages") << "\"}\n";
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
            } else if(op=="orbit" || op=="look" || op=="look-hold" || op=="pan") {
                const auto r=region("viewport.surface");
                int dx=60,dy=20;row>>dx>>dy;
                const bool look=op=="look" || op=="look-hold";
                pointer((r.left+r.right)/2,(r.top+r.bottom)/2);mouse(look?MOUSEEVENTF_RIGHTDOWN:MOUSEEVENTF_MIDDLEDOWN);pause(50);
                for(int i=1;i<=20;++i) pointer((r.left+r.right)/2+dx*i/20,(r.top+r.bottom)/2+dy*i/20);
                if (op=="look-hold") {
                    pause(100);region("viewport.surface");const auto heldCamera=lastCamera;
                    pause(600);region("viewport.surface");
                    if (lastCamera!=heldCamera) throw std::runtime_error("Camera drifted while the right button was held stationary");
                }
                mouse(look?MOUSEEVENTF_RIGHTUP:MOUSEEVENTF_MIDDLEUP);
            } else if(op=="click-at" || op=="slide") {
                std::string id;float x=0.5f,y=0.5f;row>>std::quoted(id)>>x>>y;const auto r=region(id);
                pointer(static_cast<LONG>(r.left+(r.right-r.left)*x),static_cast<LONG>(r.top+(r.bottom-r.top)*y));
                mouse(MOUSEEVENTF_LEFTDOWN);pause(100);mouse(MOUSEEVENTF_LEFTUP);pause(100);
            } else if(op=="gizmo-rotate") {
                const auto r=region("viewport.gizmo-center");const auto surface=region("viewport.surface");
                const LONG cx=(r.left+r.right)/2,cy=(r.top+r.bottom)/2;
                bool hit=false;POINT start{};
                // Discover a real ring through upstream hit testing. No
                // transform values or gizmo internals are injected by QA.
                for(int radius=std::max(10L,(surface.bottom-surface.top)/25);radius<(surface.bottom-surface.top)/7 && !hit;radius+=3)
                    for(int angle=0;angle<360 && !hit;angle+=45) {
                        const double theta=angle*3.141592653589793/180;
                        start={cx+LONG(radius*std::cos(theta)),cy+LONG(radius*std::sin(theta))};pointer(start.x,start.y);pause(35);
                        try {region("viewport.gizmo-hover");hit=true;}catch(const std::runtime_error&){}
                    }
                if(!hit)throw std::runtime_error("No rotation ring accepted mouse hover");
                pointer(start.x,start.y);mouse(MOUSEEVENTF_LEFTDOWN);pause(70);
                const double theta=std::atan2(double(start.y-cy),double(start.x-cx)),radius=std::hypot(double(start.x-cx),double(start.y-cy));
                for(int i=1;i<=30;++i)pointer(cx+LONG(radius*std::cos(theta+i*.025)),cy+LONG(radius*std::sin(theta+i*.025)));
                mouse(MOUSEEVENTF_LEFTUP);
            } else if(op=="detach" || op=="axis-drag" || op=="axis-drag-cancel") {
                std::string id;int dx=0,dy=0;row>>std::quoted(id)>>dx>>dy;const auto r=region(id);
                const auto origin=activeWindow;POINT start{(r.left+r.right)/2,(r.top+r.bottom)/2};
                POINT desktopStart=start;ClientToScreen(activeWindow,&desktopStart);
                pointer(start.x,start.y);mouse(MOUSEEVENTF_LEFTDOWN);pause(70);
                for(int i=1;i<=30;++i){POINT point{desktopStart.x+dx*i/30,desktopStart.y+dy*i/30};ScreenToClient(activeWindow,&point);pointer(point.x,point.y);}
                if(op=="axis-drag-cancel") {key(VK_ESCAPE);pause(100);key(VK_ESCAPE,true);pause(50);}
                mouse(MOUSEEVENTF_LEFTUP);activeWindow=origin;
            } else if(op=="redock") {
                std::string source,target;row>>std::quoted(source)>>std::quoted(target);const auto destination=region(target);
                POINT to{(destination.left+destination.right)/2,(destination.top+destination.bottom)/2};ClientToScreen(activeWindow,&to);
                const auto r=region(source);POINT from{(r.left+r.right)/2,(r.top+r.bottom)/2};
                POINT desktopFrom=from;ClientToScreen(activeWindow,&desktopFrom);
                pointer(from.x,from.y);mouse(MOUSEEVENTF_LEFTDOWN);pause(70);
                for(int i=1;i<=30;++i){POINT point{desktopFrom.x+(to.x-desktopFrom.x)*i/30,desktopFrom.y+(to.y-desktopFrom.y)*i/30};ScreenToClient(activeWindow,&point);pointer(point.x,point.y);}
                pause(400);mouse(MOUSEEVENTF_LEFTUP);
            } else if(op=="detached" || op=="docked") {
                std::string id;row>>std::quoted(id);region(id);
                if((activeWindow!=window)!=(op=="detached"))throw std::runtime_error("Unexpected panel platform window: "+id);
            } else if(op=="close-panel") {
                std::string id;row>>std::quoted(id);region(id);
                if(activeWindow==window)throw std::runtime_error("Panel is not detached: "+id);
                PostMessageW(activeWindow,WM_CLOSE,0,0);pause(300);
                if(!IsWindow(window))throw std::runtime_error("Closing a panel closed the editor");
            } else if(op=="typed") {
                std::string name,expected;row>>std::quoted(name)>>std::quoted(expected);region("command:play");
                std::ifstream file(snapshot);std::string item;bool found=false;
                while(std::getline(file,item)) {
                    std::istringstream value(item);std::string kind,key,text;value>>kind>>std::quoted(key)>>std::quoted(text);
                    if(kind!="typed"||key!=name)continue;
                    if(text==expected){found=true;break;}
                    std::istringstream actual(text),wanted(expected);double a=0,b=0;bool same=true;int count=0;
                    while(wanted>>b){if(!(actual>>a)||std::abs(a-b)>1e-5*std::max(1.0,std::abs(b))){same=false;break;}++count;}
                    actual>>std::ws;wanted>>std::ws;
                    found=same&&count>0&&actual.eof()&&wanted.eof();
                }
                if(!found)throw std::runtime_error("Typed property mismatch: "+name+" expected "+expected);
            } else if(op=="remember-property" || op=="property-changed" || op=="property-unchanged") {
                std::string name;row>>std::quoted(name);region("command:play");
                std::ifstream current(snapshot);std::string text,key,value;bool found=false;
                while(std::getline(current,text)) {std::istringstream fields(text);fields>>key;if(key!="typed")continue;fields>>std::quoted(key)>>std::quoted(value);if(key==name){found=true;break;}}
                if(!found)throw std::runtime_error("No typed property: "+name);
                if(op=="remember-property")rememberedProperties[name]=value;
                else {
                    const auto saved=rememberedProperties.find(name);
                    if(saved==rememberedProperties.end())throw std::runtime_error("Property has not been remembered: "+name);
                    if((saved->second==value)!=(op=="property-unchanged"))throw std::runtime_error("Property change assertion failed: "+name+" before="+saved->second+" after="+value);
                }
            } else if(op=="capture-window") {
                std::string id,name;row>>std::quoted(id)>>name;region(id);capture(name);
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
                for(wchar_t c:wide){
                    if (nativeKeyboard || queuedKeyboard) { key(static_cast<WORD>(c),false,true); pause(40); continue; }
                    const SHORT mapped=VkKeyScanW(c);
                    const BYTE modifiers=static_cast<BYTE>(mapped == -1 ? 0xFF : HIBYTE(mapped));
                    if(mapped != -1 && (modifiers & static_cast<BYTE>(~1u)) == 0) {
                        if((modifiers & 1u) != 0) key(VK_SHIFT,false);
                        key(LOBYTE(mapped),false);
                        key(LOBYTE(mapped),true);
                        if((modifiers & 1u) != 0) key(VK_SHIFT,true);
                    } else {
                        key(static_cast<WORD>(c),false,true);
                    }
                    pause(40);
                }
            } else if(op=="wheel") {int delta=0;row>>delta;mouse(MOUSEEVENTF_WHEEL,static_cast<DWORD>(delta*WHEEL_DELTA));}
            else if(op=="resize") {std::wstring size;std::string token;row>>token;size.assign(token.begin(),token.end());Size value{};if(!parse_size(size,value)||!resize_client(window,value))throw std::runtime_error("Resize failed");}
            else if(op=="capture") {std::string name;row>>name;region("command:play");capture(name);}
            else if(op=="exists"||op=="missing") {std::string path;row>>std::quoted(path);const bool exists=std::filesystem::exists(std::filesystem::u8path(path));if(exists!=(op=="exists"))throw std::runtime_error("Filesystem assertion failed: "+path);}
            else if(op=="state") {std::string expected;row>>std::quoted(expected);region("command:play");if(lastState.find(expected)==std::string::npos)throw std::runtime_error("State assertion failed: "+expected+"; "+lastState);}
            else if(op=="object-name") {std::string expected;row>>std::quoted(expected);region("command:play");if(lastObject!=expected)throw std::runtime_error("Object name mismatch: expected "+expected+", got "+lastObject);}
            else if(op=="remember-camera") {region("command:play");rememberedCamera=lastCamera;if(rememberedCamera.empty())throw std::runtime_error("No active camera");}
            else if(op=="camera-changed") {region("command:play");if(lastCamera==rememberedCamera || lastCamera.empty())throw std::runtime_error("Camera did not change");}
            else if(op=="camera-look" || op=="camera-pan-plane") {
                region("command:play");
                const auto parse=[](const std::string& line) {std::istringstream in(line);std::string key;in>>key;std::array<double,7> v{};for(auto& x:v)in>>x;return v;};
                const auto before=parse(rememberedCamera),after=parse(lastCamera);
                double movement=0,rotation=0;for(int i=0;i<3;++i)movement+=std::pow(after[i]-before[i],2);for(int i=3;i<7;++i)rotation+=std::pow(after[i]-before[i],2);
                if(op=="camera-look") {if(movement>1e-6 || rotation<1e-8)throw std::runtime_error("Mouse look translated the camera or did not rotate");}
                else {
                    const double x=before[3],y=before[4],z=before[5],w=before[6];
                    const double along=(after[0]-before[0])*2*(x*z+w*y)+(after[1]-before[1])*2*(y*z-w*x)+(after[2]-before[2])*(1-2*(x*x+y*y));
                    if(movement<1e-6 || rotation>1e-8 || std::abs(along)>0.001)throw std::runtime_error("Middle drag left the camera plane or rotated the camera");
                }
            } else if(op=="framebuffer-native") {
                const auto r=region("viewport.surface");
                const auto at=lastState.find("framebuffer=");int width=0,height=0;char separator=0;
                if(at!=std::string::npos) {std::istringstream value(lastState.substr(at+12));value>>width>>separator>>height;}
                if(separator!='x' || std::abs(width-(r.right-r.left))>1 || std::abs(height-(r.bottom-r.top))>1)throw std::runtime_error("World framebuffer does not match physical viewport pixels");
            }
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
