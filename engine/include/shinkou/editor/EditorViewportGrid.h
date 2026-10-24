#pragma once
#include "shinkou/render/RenderScene.h"
namespace shinkou::editor {
class EditorViewportGrid {
    render::ResourceHandle vertex_{},fragment_{},pipeline_{};
public:
    bool record(render::Renderer&,const render::RenderView&,render::ResourceHandle color,render::ResourceHandle depth);
    void release(render::Renderer&);
};
}
