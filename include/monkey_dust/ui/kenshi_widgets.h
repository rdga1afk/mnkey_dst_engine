#pragma once
// The three widget classes Kenshi adds to MyGUI (type="Slider", "ListScrollBar", "MultiSlider" in its layouts and templates, kenshi_templates.xml): template widgets whose children
// (ElementText, Slider, NumberText, Start / End ...) come from the ResourceLayout named by the skin. Names and signatures follow the exe's use (CharacterEditWindow, RTTI of the delegates);
// the behaviour is written from the templates and from how the exe fills them (docs/spec/ui/character_editor.md).
#include <MyGUI.h>
#include <string>
#include <vector>

namespace mdui {

class Slider : public MyGUI::Widget {
    MYGUI_RTTI_DERIVED(Slider)
public:
    Slider();
    void setCaption(const MyGUI::UString& text);
    void setScrollRange(int min, int max);      // inclusive
    void setScrollPosition(int value);          // does not raise the event
    int getScrollPosition() const;
    MyGUI::delegates::CMultiDelegate2<Slider*, int> eventScrollChangePosition;
protected:
    void initialiseOverride() override;
    void shutdownOverride() override;
private:
    void notifyScroll(MyGUI::ScrollBar*, size_t pos);
    void ShowNumber();
    MyGUI::TextBox* text_ = nullptr;
    MyGUI::ScrollBar* scroll_ = nullptr;
    MyGUI::TextBox* number_ = nullptr;
    int min_ = 0, max_ = 100;
};

class ListScrollBar : public MyGUI::Widget {
    MYGUI_RTTI_DERIVED(ListScrollBar)
public:
    ListScrollBar();
    void setCaption(const MyGUI::UString& text);
    void setValues(const std::vector<std::string>& values);
    void setValueSelected(const std::string& value);     // no event; unknown value: nothing selected
    int getSelectedIndex() const { return index_; }
    void step(int direction);                            // +1 / -1 as the End / Start button does (wraps around)
    MyGUI::delegates::CMultiDelegate3<ListScrollBar*, const std::string&, int> eventValueChanged;   // (widget, value, index)
protected:
    void initialiseOverride() override;
    void shutdownOverride() override;
private:
    void notifyStep(MyGUI::Widget* sender);
    void Select(int i, bool raise);
    MyGUI::Button* start_ = nullptr;
    MyGUI::Button* end_ = nullptr;
    MyGUI::TextBox* text_ = nullptr;
    MyGUI::TextBox* number_ = nullptr;
    MyGUI::TextBox* value_ = nullptr;
    std::vector<std::string> values_;
    int index_ = -1;
};

class MultiSlider : public MyGUI::Widget {
    MYGUI_RTTI_DERIVED(MultiSlider)
public:
    MultiSlider();
    void setCaption(const MyGUI::UString& text);
    size_t getSlidersCount() const { return count_; }
    Slider* getSlider(size_t i);
    MyGUI::delegates::CMultiDelegate4<MultiSlider*, Slider*, int, int> eventScrollChangePosition;   // (widget, slider, position, slider index)
protected:
    void initialiseOverride() override;
    void shutdownOverride() override;
private:
    void notifySlider(Slider* s, int pos);
    MyGUI::TextBox* name_ = nullptr;
    size_t count_ = 0;
};

void RegisterKenshiWidgets();   // before the layouts are loaded

} // namespace mdui
