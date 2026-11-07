// Общий трек радио. Движком занимается Lua (PDCoopRecon/Scripts/radio.lua), мод возит имя трека по сети:
//  Host: radio.lua дописывает «вид|имя» в coop/radio_host.txt → мод шлёт Client-у (RadioTrack);
//  Client: мод дописывает полученное в coop/radio_cmd.txt → radio.lua включает этот трек.
// Вид: file — свой mp3 из Custom\Radio (имя файла), stock — штатный трек (Title).
#pragma once
#include <filesystem>
#include <string>

namespace RadioSync
{
    void init(const std::filesystem::path& coop_dir);
    void tick(bool is_host);
    void on_radio(const std::string& kind, const std::string& name);
} // namespace RadioSync
