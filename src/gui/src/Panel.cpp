#include "Panel.h"
#include "Drawing.h"

namespace MFM {

  Panel::Panel(u32 width, u32 height)
  {
    SetDimensions(width, height);

    m_forward = m_backward = 0;
    m_parent = m_top = 0;

    m_name = 0;

    m_bgColor = Drawing::BLACK;
    m_fgColor = Drawing::YELLOW;
  }

  Panel::~Panel()
  {

    // Eject any content in us
    while (m_top)
      Remove(m_top);

    // Eject us from our parent
    if (m_parent)
      m_parent->Remove(this);
  }

  void Panel::Indent(FILE * file, u32 count)
  {
    for (u32 i = 0; i < count; ++i)
      fprintf(file," ");
  }
  void Panel::Print(FILE * file, u32 indent) const
  {
    Indent(file,indent);
    fprintf(file,"[");
    if (GetName()) fprintf(file,"%s:", GetName());
    fprintf(file,"%p",(void*) this);
    fprintf(file,"(%d,%d)%dx%d,bg:%08x,fg:%08x",
            m_rect.GetX(),
            m_rect.GetY(),
            m_rect.GetWidth(),
            m_rect.GetHeight(),
            m_bgColor,
            m_fgColor);
    if (m_top) {
      Panel * p = m_top;
      fprintf(file,"\n");
      do {
        p = p->m_forward;
        p->Print(file, indent+2);
      } while (p != m_top);
      Indent(file,indent);
    }
    fprintf(file,"]\n");
  }

  void Panel::Insert(Panel * child, Panel * after)
  {
    if (!child) FAIL(NULL_POINTER);
    if (child->m_parent) FAIL(ILLEGAL_ARGUMENT);

    if (!m_top) {

      if (after) FAIL(ILLEGAL_ARGUMENT);
      m_top = child->m_forward = child->m_backward = child;

    } else {

      if (!after) after = m_top;
      else if (after->m_parent != this) FAIL(ILLEGAL_ARGUMENT);

      child->m_forward = after->m_forward;
      child->m_backward = after;
      after->m_forward->m_backward = child;
      after->m_forward = child;
    }

    child->m_parent = this;
  }

  void Panel::Remove(Panel * child)
  {
    if (!child) FAIL(NULL_POINTER);
    if (child->m_parent != this) FAIL(ILLEGAL_ARGUMENT);

    if (child->m_forward == child)  // Single elt list
      m_top = 0;
    else {
      if (m_top == child)
        m_top = child->m_forward;
      child->m_forward->m_backward = child->m_backward;
      child->m_backward->m_forward = child->m_forward;
    }
    child->m_parent = 0;
    child->m_forward = 0;
    child->m_backward = 0;
  }

  void Panel::SetDimensions(u32 width, u32 height)
  {
    m_rect.SetWidth(width);
    m_rect.SetHeight(height);
  }

  const UPoint & Panel::GetDimensions() const
  {
    return m_rect.GetSize();
  }

  void Panel::SetRenderPoint(const SPoint & renderPt)
  {
    m_rect.SetPosition(renderPt);
  }

  void Panel::Paint(Drawing & drawing)
  {
    Rect old, cur;
    drawing.GetWindow(old);
    drawing.TransformWindow(m_rect);
    drawing.GetWindow(cur);

    PaintComponent(drawing);
    PaintBorder(drawing);

    drawing.SetWindow(cur);
    PaintChildren(drawing);

    drawing.SetWindow(old);
  }

  void Panel::PaintChildren(Drawing & drawing)
  {
    if (m_top) {
      Rect cur;
      drawing.GetWindow(cur);

      Panel * p = m_top;
      do {
        p = p->m_forward;
        drawing.SetWindow(cur);
        p->Paint(drawing);
      } while (p != m_top);
    }
  }

  void Panel::PaintComponent(Drawing & drawing)
  {
    drawing.SetForeground(m_fgColor);
    drawing.SetBackground(m_bgColor);
    drawing.Clear();
  }

  void Panel::PaintBorder(Drawing & drawing)
  {
    drawing.SetForeground(m_fgColor);
    drawing.DrawRectangle(Rect(SPoint(),m_rect.GetSize()));
  }

  bool Panel::Dispatch(SDL_Event & event, const Rect & existing)
  {
    SPoint at;
    SDL_MouseButtonEvent * button = 0;
    SDL_MouseMotionEvent * motion = 0;

    switch (event.type) {
    case SDL_MOUSEMOTION:
      motion = &event.motion;
      at.SetX(motion->x);
      at.SetY(motion->y);
      break;
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEBUTTONDOWN:
      button = &event.button;
      at.SetX(button->x);
      at.SetY(button->y);
      break;
    default:
      return false;
    }

    Rect newRect;
    Drawing::TransformWindow(existing, m_rect, newRect);

    if (!newRect.Contains(at))
      return false;

    if (m_top) {

      // Scan kids from top down so more visible gets first crack
      Panel * p = m_top;
      do {
        if (p->Dispatch(event, newRect))
          return true;
        p = p->m_backward;
      } while (p != m_top);
    }

    // Here the hit is in us and none of our descendants wanted it.
    // So it's ours if we do.

    switch (event.type) {
    case SDL_MOUSEMOTION:
      return Handle(*motion);
    case SDL_MOUSEBUTTONUP:
      printf("Panel Button Up");
      Print(stdout);
      // FALL THROUGH
    case SDL_MOUSEBUTTONDOWN:
      return Handle(*button);
    default:
      break;
    }

    return false;
  }

} /* namespace MFM */
