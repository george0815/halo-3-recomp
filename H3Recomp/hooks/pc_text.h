// PC wording for the game's English text (pc_text.cpp).

#pragma once

namespace h3::hooks
{
    // Once per presented frame: rewords the active language pack after a
    // map loads it.
    void UpdatePcText();
}
