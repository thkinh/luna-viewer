#include "lunaviewer/ui.hpp"
#include <Qt>
#include <algorithm>
#include <cmath>
#include <limits>
namespace lunaviewer {
QRectF frame_rect(const Geometry& g, const QSizeF& view) {
    if (g.width <= 0 || g.height <= 0 || view.width() <= 0 || view.height() <= 0) return {};
    const double scale = std::min(view.width()/g.width, view.height()/g.height);
    const QSizeF size(g.width*scale, g.height*scale);
    return QRectF(QPointF((view.width()-size.width())/2, (view.height()-size.height())/2), size);
}
bool map_position(const Geometry& g, const QSizeF& view, const QPointF& point, QPoint& remote, bool clamp) {
    const auto rect = frame_rect(g, view);
    if (rect.isEmpty() || !std::isfinite(point.x()) || !std::isfinite(point.y())) return false;
    if (!clamp && (point.x()<rect.left() || point.x()>=rect.right() || point.y()<rect.top() || point.y()>=rect.bottom())) return false;
    const auto x = std::max(0.0, std::min(double(g.width-1), std::floor((point.x()-rect.left())*g.width/rect.width())));
    const auto y = std::max(0.0, std::min(double(g.height-1), std::floor((point.y()-rect.top())*g.height/rect.height())));
    const auto rx = double(g.left)+x, ry = double(g.top)+y;
    if (rx<std::numeric_limits<int>::min() || rx>std::numeric_limits<int>::max() || ry<std::numeric_limits<int>::min() || ry>std::numeric_limits<int>::max()) return false;
    remote = QPoint(int(rx),int(ry));
    return true;
}
bool map_key(int key, bool keypad, InputEvent& out) {
    out = InputEvent{}; out.kind = InputEvent::Kind::Key;
    auto mapped = [&](unsigned vk, unsigned scan, bool extended=false) {
        out.virtual_key=vk; out.scan_code=scan; out.extended=extended; return true;
    };
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        static const unsigned scans[] = {0x1e,0x30,0x2e,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,0x31,0x18,0x19,0x10,0x13,0x1f,0x14,0x16,0x2f,0x11,0x2d,0x15,0x2c};
        return mapped(unsigned('A'+key-Qt::Key_A), scans[key-Qt::Key_A]);
    }
    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        static const unsigned numpad[] = {0x52,0x4f,0x50,0x51,0x4b,0x4c,0x4d,0x47,0x48,0x49};
        const int digit=key-Qt::Key_0;
        return mapped((keypad?0x60:0x30)+digit, keypad?numpad[digit]:(digit?digit+1:0x0b));
    }
    if (key>=Qt::Key_F1 && key<=Qt::Key_F12) {
        const unsigned n=unsigned(key-Qt::Key_F1);
        return mapped(0x70+n,n<10?0x3b+n:0x57+n-10);
    }
    if (keypad) {
        switch(key) {
        case Qt::Key_Plus:return mapped(0x6b,0x4e);
        case Qt::Key_Minus:return mapped(0x6d,0x4a);
        case Qt::Key_Asterisk:return mapped(0x6a,0x37);
        case Qt::Key_Slash:return mapped(0x6f,0x35,true);
        case Qt::Key_Period:case Qt::Key_Comma:return mapped(0x6e,0x53);
        default:break;
        }
    }
    switch(key) {
    case Qt::Key_Escape:return mapped(0x1b,0x01);
    case Qt::Key_Tab:case Qt::Key_Backtab:return mapped(0x09,0x0f);
    case Qt::Key_Backspace:return mapped(0x08,0x0e);
    case Qt::Key_Return:return mapped(0x0d,0x1c);
    case Qt::Key_Enter:return mapped(0x0d,0x1c,true);
    case Qt::Key_Space:return mapped(0x20,0x39);
    case Qt::Key_Shift:return mapped(0x10,0x2a);
    case Qt::Key_Control:return mapped(0x11,0x1d);
    case Qt::Key_Alt:return mapped(0x12,0x38);
    case Qt::Key_AltGr:return mapped(0xa5,0x38,true);
    case Qt::Key_Meta:case Qt::Key_Super_L:return mapped(0x5b,0x5b,true);
    case Qt::Key_Super_R:return mapped(0x5c,0x5c,true);
    case Qt::Key_Menu:return mapped(0x5d,0x5d,true);
    case Qt::Key_CapsLock:return mapped(0x14,0x3a);
    case Qt::Key_NumLock:return mapped(0x90,0x45,true);
    case Qt::Key_ScrollLock:return mapped(0x91,0x46);
    case Qt::Key_Print:return mapped(0x2c,0x37,true);
    case Qt::Key_Insert:return mapped(0x2d,0x52,!keypad);
    case Qt::Key_Delete:return mapped(0x2e,0x53,!keypad);
    case Qt::Key_Home:return mapped(0x24,0x47,!keypad);
    case Qt::Key_End:return mapped(0x23,0x4f,!keypad);
    case Qt::Key_PageUp:return mapped(0x21,0x49,!keypad);
    case Qt::Key_PageDown:return mapped(0x22,0x51,!keypad);
    case Qt::Key_Left:return mapped(0x25,0x4b,!keypad);
    case Qt::Key_Right:return mapped(0x27,0x4d,!keypad);
    case Qt::Key_Up:return mapped(0x26,0x48,!keypad);
    case Qt::Key_Down:return mapped(0x28,0x50,!keypad);
    case Qt::Key_Exclam:return mapped(0x31,0x02);
    case Qt::Key_At:return mapped(0x32,0x03);
    case Qt::Key_NumberSign:return mapped(0x33,0x04);
    case Qt::Key_Dollar:return mapped(0x34,0x05);
    case Qt::Key_Percent:return mapped(0x35,0x06);
    case Qt::Key_AsciiCircum:return mapped(0x36,0x07);
    case Qt::Key_Ampersand:return mapped(0x37,0x08);
    case Qt::Key_Asterisk:return mapped(0x38,0x09);
    case Qt::Key_ParenLeft:return mapped(0x39,0x0a);
    case Qt::Key_ParenRight:return mapped(0x30,0x0b);
    case Qt::Key_Minus:case Qt::Key_Underscore:return mapped(0xbd,0x0c);
    case Qt::Key_Equal:case Qt::Key_Plus:return mapped(0xbb,0x0d);
    case Qt::Key_BracketLeft:case Qt::Key_BraceLeft:return mapped(0xdb,0x1a);
    case Qt::Key_BracketRight:case Qt::Key_BraceRight:return mapped(0xdd,0x1b);
    case Qt::Key_Backslash:case Qt::Key_Bar:return mapped(0xdc,0x2b);
    case Qt::Key_Semicolon:case Qt::Key_Colon:return mapped(0xba,0x27);
    case Qt::Key_Apostrophe:case Qt::Key_QuoteDbl:return mapped(0xde,0x28);
    case Qt::Key_QuoteLeft:case Qt::Key_AsciiTilde:return mapped(0xc0,0x29);
    case Qt::Key_Comma:case Qt::Key_Less:return mapped(0xbc,0x33);
    case Qt::Key_Period:case Qt::Key_Greater:return mapped(0xbe,0x34);
    case Qt::Key_Slash:case Qt::Key_Question:return mapped(0xbf,0x35);
    default:return false;
    }
}
} // namespace lunaviewer
