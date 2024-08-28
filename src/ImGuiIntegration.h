#pragma once

namespace ImGuiIntegration
{
    bool Start(void callback());
    void Show(bool toShow);
    void BlockInput(bool toBlock, bool toBlockClicks = false); //If block clicks is true, will still block those even if toBlock is false
}