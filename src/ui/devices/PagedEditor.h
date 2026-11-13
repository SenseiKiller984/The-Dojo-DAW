#pragma once

#include "ui/devices/ParameterControl.h"
#include "ui/theme/DojoTheme.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

namespace dojo::ui
{

/** A device editor in pages (tabs): each a grid of parameters in two rows, with an optional display
    (curve, meter, scope) beside it. */
class PagedEditor final : public juce::Component
{
public:
    struct Page
    {
        juce::String title;
        std::vector<std::pair<juce::String, juce::String>> parameters; ///< id, caption
        std::function<std::unique_ptr<juce::Component>()> makeDisplay;
        int displayWidth = 0;
    };

    static constexpr int kTabHeight = 22;
    static constexpr int kRows = 2;

    PagedEditor(juce::AudioProcessor& processor, std::vector<Page> pages)
        : processor_(processor)
        , pages_(std::move(pages))
    {
        for (std::size_t i = 0; i < pages_.size(); ++i)
        {
            auto tab = std::make_unique<juce::TextButton>(pages_[i].title);
            tab->setRadioGroupId(4242);
            tab->setClickingTogglesState(true);
            tab->setWantsKeyboardFocus(false);
            tab->setColour(juce::TextButton::buttonOnColourId, theme::colour::controlHover);
            tab->onClick = [this, i] { showPage(i); };
            addAndMakeVisible(*tab);
            tabs_.push_back(std::move(tab));
        }
        addAndMakeVisible(grid_);
        showPage(0);
    }

    /** Width that fits the widest page in two rows (and every tab). */
    static int preferredWidth(const std::vector<Page>& pages)
    {
        int width = 0;
        int tabs = 0;
        for (const auto& page : pages)
        {
            const int columns = (static_cast<int>(page.parameters.size()) + kRows - 1) / kRows;
            width = std::max(width, columns * ParameterControl::kWidth + page.displayWidth + 28); // card margins
            tabs += tabWidth(page.title);
        }
        return std::max(width, tabs + 8);
    }

    void showPage(std::size_t index)
    {
        page_ = std::min(index, pages_.size() - 1);
        for (std::size_t i = 0; i < tabs_.size(); ++i)
            tabs_[i]->setToggleState(i == page_, juce::dontSendNotification);

        grid_.clear();
        for (const auto& [id, caption] : pages_[page_].parameters)
            if (auto* parameter = findParameter(processor_, id))
                grid_.add(*parameter, caption);

        display_.reset();
        if (pages_[page_].makeDisplay)
        {
            display_ = pages_[page_].makeDisplay();
            addAndMakeVisible(*display_);
        }
        resized();
    }

    [[nodiscard]] std::size_t getPage() const noexcept { return page_; }
    [[nodiscard]] std::size_t getNumPages() const noexcept { return pages_.size(); }
    [[nodiscard]] const juce::String& getPageTitle(std::size_t index) const { return pages_[index].title; }
    /** Parameter ids on any page that the processor does not have (tests). */
    [[nodiscard]] juce::StringArray getMissingParameters() const
    {
        juce::StringArray missing;
        for (const auto& page : pages_)
            for (const auto& [id, caption] : page.parameters)
                if (findParameter(processor_, id) == nullptr)
                    missing.add(page.title + ": " + id);
        return missing;
    }

    void resized() override
    {
        auto area = getLocalBounds();
        auto tabs = area.removeFromTop(kTabHeight);
        for (std::size_t i = 0; i < tabs_.size(); ++i)
            tabs_[i]->setBounds(tabs.removeFromLeft(tabWidth(pages_[i].title)).reduced(1, 0));
        area.removeFromTop(4);
        if (display_ != nullptr)
            display_->setBounds(area.removeFromLeft(pages_[page_].displayWidth).reduced(2));
        grid_.setBounds(area);
    }

private:
    static int tabWidth(const juce::String& title) { return 18 + 7 * title.length(); }

    juce::AudioProcessor& processor_;
    std::vector<Page> pages_;
    std::vector<std::unique_ptr<juce::TextButton>> tabs_;
    ParameterGrid grid_;
    std::unique_ptr<juce::Component> display_;
    std::size_t page_ = 0;
};

} // namespace dojo::ui
