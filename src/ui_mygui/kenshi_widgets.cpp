#include <monkey_dust/ui/kenshi_widgets.h>

#include <algorithm>

namespace mdui {

// ── Slider ───────────────────────────────────────────────────────────────────────────────────────────────────
Slider::Slider() {}

void Slider::initialiseOverride() {
    Base::initialiseOverride();
    assignWidget(text_, "ElementText");
    assignWidget(scroll_, "Slider");
    assignWidget(number_, "NumberText");
    if (scroll_) scroll_->eventScrollChangePosition += MyGUI::newDelegate(this, &Slider::notifyScroll);
}

void Slider::shutdownOverride() {
    if (scroll_) scroll_->eventScrollChangePosition -= MyGUI::newDelegate(this, &Slider::notifyScroll);
    Base::shutdownOverride();
}

void Slider::setCaption(const MyGUI::UString& t) { if (text_) text_->setCaption(t); }

void Slider::setScrollRange(int lo, int hi) {
    min_ = lo; max_ = std::max(lo, hi);
    if (scroll_) scroll_->setScrollRange(size_t(max_ - min_ + 1));
    ShowNumber();
}

void Slider::setScrollPosition(int v) {
    v = std::max(min_, std::min(max_, v));
    if (scroll_) scroll_->setScrollPosition(size_t(v - min_));
    ShowNumber();
}

int Slider::getScrollPosition() const { return scroll_ ? min_ + int(scroll_->getScrollPosition()) : min_; }

void Slider::ShowNumber() { if (number_) number_->setCaption(std::to_string(getScrollPosition())); }

void Slider::notifyScroll(MyGUI::ScrollBar*, size_t pos) {
    ShowNumber();
    eventScrollChangePosition(this, min_ + int(pos));
}

// ── ListScrollBar ────────────────────────────────────────────────────────────────────────────────────────────
ListScrollBar::ListScrollBar() {}

void ListScrollBar::initialiseOverride() {
    Base::initialiseOverride();
    assignWidget(start_, "Start");
    assignWidget(end_, "End");
    assignWidget(text_, "ElementText");
    assignWidget(number_, "NumberText");
    assignWidget(value_, "ElementValueText");
    if (start_) start_->eventMouseButtonClick += MyGUI::newDelegate(this, &ListScrollBar::notifyStep);
    if (end_) end_->eventMouseButtonClick += MyGUI::newDelegate(this, &ListScrollBar::notifyStep);
}

void ListScrollBar::shutdownOverride() {
    if (start_) start_->eventMouseButtonClick -= MyGUI::newDelegate(this, &ListScrollBar::notifyStep);
    if (end_) end_->eventMouseButtonClick -= MyGUI::newDelegate(this, &ListScrollBar::notifyStep);
    Base::shutdownOverride();
}

void ListScrollBar::setCaption(const MyGUI::UString& t) { if (text_) text_->setCaption(t); }

void ListScrollBar::setValues(const std::vector<std::string>& v) { values_ = v; index_ = -1; Select(-1, false); }

void ListScrollBar::setValueSelected(const std::string& v) {
    auto it = std::find(values_.begin(), values_.end(), v);
    Select(it == values_.end() ? -1 : int(it - values_.begin()), false);
}

void ListScrollBar::Select(int i, bool raise) {
    index_ = i;
    if (value_) value_->setCaption(i >= 0 ? MyGUI::UString(values_[size_t(i)]) : MyGUI::UString());
    if (number_) number_->setCaption(i >= 0 ? std::to_string(i + 1) + "/" + std::to_string(values_.size()) : "-/" + std::to_string(values_.size()));
    if (raise && i >= 0) eventValueChanged(this, values_[size_t(i)], i);
}

void ListScrollBar::notifyStep(MyGUI::Widget* sender) { step(sender == end_ ? 1 : -1); }

void ListScrollBar::step(int direction) {
    if (values_.empty()) return;
    const int n = int(values_.size());
    Select(((index_ < 0 ? (direction > 0 ? -1 : 0) : index_) + direction + n) % n, true);   // wraps around
}

// ── MultiSlider ──────────────────────────────────────────────────────────────────────────────────────────────
MultiSlider::MultiSlider() {}

void MultiSlider::initialiseOverride() {
    Base::initialiseOverride();
    assignWidget(name_, "Name");
    for (size_t i = 0;; ++i) {
        Slider* s = nullptr;
        assignWidget(s, "Slider" + std::to_string(i + 1));
        if (!s) break;
        s->eventScrollChangePosition += MyGUI::newDelegate(this, &MultiSlider::notifySlider);
        ++count_;
    }
}

void MultiSlider::shutdownOverride() {
    for (size_t i = 0; i < count_; ++i)
        if (Slider* s = getSlider(i)) s->eventScrollChangePosition -= MyGUI::newDelegate(this, &MultiSlider::notifySlider);
    Base::shutdownOverride();
}

void MultiSlider::setCaption(const MyGUI::UString& t) { if (name_) name_->setCaption(t); }

Slider* MultiSlider::getSlider(size_t i) {
    Slider* s = nullptr;
    assignWidget(s, "Slider" + std::to_string(i + 1));
    return s;
}

void MultiSlider::notifySlider(Slider* s, int pos) {
    for (size_t i = 0; i < count_; ++i)
        if (getSlider(i) == s) { eventScrollChangePosition(this, s, pos, int(i)); return; }
}

void RegisterKenshiWidgets() {
    auto& f = MyGUI::FactoryManager::getInstance();
    f.registerFactory<Slider>("Widget");
    f.registerFactory<ListScrollBar>("Widget");
    f.registerFactory<MultiSlider>("Widget");
}

} // namespace mdui
