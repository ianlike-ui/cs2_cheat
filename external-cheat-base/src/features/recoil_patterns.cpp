// ============================================================================
// recoil_patterns.cpp —— 枪械后坐弹道录制 / 回放模式表的实现
// ============================================================================
// 数据文件格式（%LOCALAPPDATA%\AegisCS2\recoil_patterns.txt）：
//   每行一把枪：武器索引|武器名|p1x,p1y;p2x,p2y;...
//   例：7|AK-47|-0.12,0.03;-0.28,0.05;...
// 子弹的坐标是"第 N+1 发的 aimPunch 累积值"（角度），回放时相邻相减得增量。
// ============================================================================

#include "recoil_patterns.hpp"
#include <Windows.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include <cwchar>

namespace
{
    std::mutex gMutex;

    std::vector<recoil_patterns::Pattern> gPatterns;

    // 录音状态（压枪线程写）
    bool gRecording = false;
    int gRecordWeaponIndex = 0;
    std::string gRecordWeaponName;
    std::vector<vec2> gRecordBuffer;

    // 数据文件路径：%LOCALAPPDATA%\AegisCS2\recoil_patterns.txt
    std::filesystem::path patternFilePath()
    {
        std::array<wchar_t, 32768> localAppData{};
        const DWORD length = GetEnvironmentVariableW(
            L"LOCALAPPDATA",
            localAppData.data(),
            static_cast<DWORD>(localAppData.size()));
        std::filesystem::path directory =
            length > 0 && length < localAppData.size()
                ? std::filesystem::path(
                    std::wstring_view(localAppData.data(), length)) /
                    L"AegisCS2"
                : std::filesystem::temp_directory_path() / L"AegisCS2";
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        return directory / L"recoil_patterns.txt";
    }
}

namespace recoil_patterns
{
    const Pattern* find(int weaponIndex)
    {
        std::lock_guard<std::mutex> lock(gMutex);
        for (const Pattern& p : gPatterns) {
            if (p.weaponIndex == weaponIndex) {
                return &p;
            }
        }
        return nullptr;
    }

    std::vector<Pattern> snapshotAll()
    {
        std::lock_guard<std::mutex> lock(gMutex);
        return gPatterns;
    }

    bool isRecording()
    {
        std::lock_guard<std::mutex> lock(gMutex);
        return gRecording;
    }

    int recordingWeaponIndex()
    {
        std::lock_guard<std::mutex> lock(gMutex);
        return gRecordWeaponIndex;
    }

    int recordingBulletCount()
    {
        std::lock_guard<std::mutex> lock(gMutex);
        return static_cast<int>(gRecordBuffer.size());
    }

    void startRecording(int weaponIndex, const std::string& weaponName)
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gRecordWeaponIndex = weaponIndex;
        gRecordWeaponName = weaponName;
        gRecordBuffer.clear();
        gRecording = true;
    }

    void recordBullet(float pitch, float yaw)
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (!gRecording) {
            return;
        }
        gRecordBuffer.push_back({ pitch, yaw });
    }

    bool stopRecording()
    {
        Pattern recorded;
        {
            std::lock_guard<std::mutex> lock(gMutex);
            if (!gRecording || gRecordBuffer.empty()) {
                gRecording = false;
                gRecordBuffer.clear();
                return false;
            }
            recorded.weaponIndex = gRecordWeaponIndex;
            recorded.weaponName = gRecordWeaponName;
            recorded.bullets = gRecordBuffer;
            gRecording = false;
            gRecordBuffer.clear();
        }

        bool stored = false;
        {
            std::lock_guard<std::mutex> lock(gMutex);
            bool replaced = false;
            for (Pattern& p : gPatterns) {
                if (p.weaponIndex == recorded.weaponIndex) {
                    p = recorded;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                gPatterns.push_back(recorded);
            }
            stored = true;
        }
        saveToDisk();
        return stored;
    }

    void cancelRecording()
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gRecording = false;
        gRecordBuffer.clear();
    }

    void removePattern(int weaponIndex)
    {
        {
            std::lock_guard<std::mutex> lock(gMutex);
            for (auto it = gPatterns.begin(); it != gPatterns.end(); ++it) {
                if (it->weaponIndex == weaponIndex) {
                    gPatterns.erase(it);
                    break;
                }
            }
        }
        saveToDisk();
    }

    void clearAll()
    {
        {
            std::lock_guard<std::mutex> lock(gMutex);
            gPatterns.clear();
        }
        saveToDisk();
    }

    void saveToDisk()
    {
        std::vector<Pattern> snapshot;
        {
            std::lock_guard<std::mutex> lock(gMutex);
            snapshot = gPatterns;
        }
        const std::filesystem::path path = patternFilePath();
        std::ofstream out(path, std::ios::trunc);
        if (!out) {
            return;
        }
        for (const Pattern& p : snapshot) {
            out << p.weaponIndex << '|' << p.weaponName << '|';
            for (size_t i = 0; i < p.bullets.size(); ++i) {
                out << p.bullets[i].x << ',' << p.bullets[i].y;
                if (i + 1 < p.bullets.size()) {
                    out << ';';
                }
            }
            out << '\n';
        }
    }

    void loadFromDisk()
    {
        const std::filesystem::path path = patternFilePath();
        std::ifstream in(path);
        if (!in) {
            return;
        }
        std::vector<Pattern> loaded;
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) {
                continue;
            }
            std::istringstream ls(line);
            Pattern p;
            std::string idxStr, name, bulletsStr;
            if (!std::getline(ls, idxStr, '|')) continue;
            if (!std::getline(ls, name, '|')) continue;
            if (!std::getline(ls, bulletsStr, '|')) continue;
            p.weaponIndex = std::atoi(idxStr.c_str());
            p.weaponName = name;
            std::istringstream bs(bulletsStr);
            std::string token;
            while (std::getline(bs, token, ';')) {
                if (token.empty()) continue;
                std::istringstream cs(token);
                std::string xs, ys;
                if (!std::getline(cs, xs, ',')) continue;
                if (!std::getline(cs, ys, ',')) continue;
                const float px = static_cast<float>(std::atof(xs.c_str()));
                const float py = static_cast<float>(std::atof(ys.c_str()));
                if (px >= -90.0f && px <= 90.0f &&
                    py >= -180.0f && py <= 180.0f) {
                    p.bullets.push_back({ px, py });
                }
            }
            if (p.weaponIndex > 0 && !p.bullets.empty()) {
                loaded.push_back(std::move(p));
            }
        }
        std::lock_guard<std::mutex> lock(gMutex);
        gPatterns = std::move(loaded);
    }
}
