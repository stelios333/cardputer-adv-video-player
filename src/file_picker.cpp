
#include <Arduino.h>
#include <SD.h>
#include "lgfx_config.h"
#include "color_conv.h"
#include "marquee.h"
#include "file_picker.h"
#include "message_box.h"
#include "keypad_input.h"

bool isH264File(const String& name)
{
    const int dot = name.lastIndexOf('.');
    if (dot < 0) {
        return false;
    }
    String ext = name.substring(dot + 1);
    ext.toLowerCase();
    return ext == "264";
}

void drawPicker(LGFX& tft, const String& dir, const std::vector<String>& names,
                const std::vector<bool>& isDir, int selected,
                Marquee& marquee)
{
    

    // Same chrome as fe_settings_menu: black clear, rgb888(0x00032303)
    // header/footer, black shadow text at (3,0)/(3,124), white copy at
    // (4,1)/(4,125), white rules at y=11 and y=123.
    tft.fillRect(0, 0, tft.width(), tft.height(), 0x0000);

    tft.fillRect(0, 0, tft.width(), 10, TFT_GRAY);
    tft.setTextSize(1);
    tft.setTextColor(0x0000);
    tft.drawString("  /\\ \\/ TO NAVIGATE | ENTER TO LOAD", 3, 0);
    tft.setTextColor(0xFFFF);
    tft.drawString("  /\\ \\/ TO NAVIGATE | ENTER TO LOAD", 4, 1);
    tft.drawFastHLine(0, 11, tft.width(), 0xFFFF);


    const int height = tft.height();
    const int count  = static_cast<int>(names.size());

    auto drawItem = [&](int index, int lineOffset) {
        if (index < 0 || index >= count) {
            return;
        }
        const bool directory = isDir[index];
        const bool highlight = (lineOffset == 0);
        const int fontSize = highlight ? 0x3c : 0x60;
        tft.setTextSize(static_cast<int>(tft.height()/fontSize));
        const int w = tft.textWidth(names[index].c_str());
        const int h = tft.fontHeight();
        const int y = (lineOffset == 0)
                          ? height / 2 - h / 2
                          : height / 2 - h / 2 + lineOffset * h;
        if (highlight) {
            // Long selected names start at their left-most position and then
            // scroll in the idle loop (see marqueeStep).
            marqueeReset(tft, marquee, w);
            tft.fillRect(0, y - 2, tft.width(), h + 4,
                       rgb888ToRgb565(0x00111111));
        }
        uint32_t color;
        if (highlight) {
            color = directory ? 0x00FFFF00 : 0x00FFFFFF;
        } else {
            color = directory ? 0x00AAAA00 : 0x00AAAAAA;
        }
        tft.setTextColor(rgb888ToRgb565(color));
        tft.drawString(names[index].c_str(),
                     kTextLeft + (highlight ? marquee.scroll : 0), y);
    };

    // The picker is a five-entry carousel: +/-1 sit three text lines from the
    // centre and +/-2 sit five lines out.
    drawItem(selected - 2, -5);
    drawItem(selected + 2, +5);
    drawItem(selected + 3, +7);
    drawItem(selected - 1, -3);
    drawItem(selected + 1, +3);
    drawItem(selected, 0);
}

void filePicker(LGFX& tft, char* out, size_t outSize)
{
    String currentDir = "/";
    int selected = 0;
    keyboard.resetKeyStateChangedAll();
    while (true) {
        std::vector<String> names;
        std::vector<bool> isDir;

        File dir = SD.open(currentDir);
        if (!dir) {
            messageBoxDraw(tft, -15, "SD Card has no", "Directory/Filename", 0xFFFF,
                           0xFFFF, 2, 2, 4);
            delay(1000);
            strncpy(out, "/", outSize);
            return;
        }

        File entry = dir.openNextFile();
        while (entry) {
            String name = entry.name();
            // Some cores return the full path; keep only the leaf name.
            const int slash = name.lastIndexOf('/');
            if (slash >= 0) {
                name = name.substring(slash + 1);
            }

            if (name != "System Volume Information" &&
                names.size() < static_cast<size_t>(kMaxFiles)) {
                if (entry.isDirectory()) {
                    names.push_back(name);
                    isDir.push_back(true);
                } else if (isH264File(name)) {
                    names.push_back(name);
                    isDir.push_back(false);
                }
            }
            entry.close();
            entry = dir.openNextFile();
        }
        dir.close();

        // Directories first, then case-insensitive name order.
        std::vector<int> order(names.size());
        for (size_t i = 0; i < order.size(); i++) order[i] = static_cast<int>(i);
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            if (isDir[a] != isDir[b]) return isDir[a] > isDir[b];
            return strcasecmp(names[a].c_str(), names[b].c_str()) < 0;
        });

        std::vector<String> sortedNames;
        std::vector<bool> sortedDirs;
        for (int i : order) {
            sortedNames.push_back(names[i]);
            sortedDirs.push_back(isDir[i]);
        }
        names.swap(sortedNames);
        isDir.swap(sortedDirs);

        // Allow navigating back to the parent directory.
        if (currentDir != "/") {
            names.insert(names.begin(), "..");
            isDir.insert(isDir.begin(), true);
        }

        if (names.empty()) {
            messageBoxDraw(tft, -15, "SD Card has no", ".h264 files", 0xFFFF,
                           0xFFFF, 2, 2, 4);
            delay(1000);
            strncpy(out, "/", outSize);
            return;
        }


        Marquee pickerMarquee;
        auto lDrawPicker = [&]() {
            if (selected < 0) selected = static_cast<int>(names.size()) - 1;
            if (selected >= static_cast<int>(names.size())) selected = 0;
            drawPicker(tft, currentDir, names, isDir, selected, pickerMarquee);
        };
        lDrawPicker();
        bool done = false;
    
        while (!done) {
            keyboard.updateKeyStatus();
            if (!keyboard.anyKey()) {
                // Animate a long selected name while the picker is idle.
                marqueeStep(tft, pickerMarquee, names[selected].c_str(),
                            isDir[selected] ? 0x00FFFF00 : 0x00FFFFFF);
                continue;
            }
            else {
                if (keyboard.keyPressed(VP_KEY_ENTER)) {
                    if (isDir[selected]) {
                        if (names[selected] == "..") {
                            const int slash = currentDir.lastIndexOf('/');
                            currentDir = (slash <= 0) ? "/" : currentDir.substring(0, slash);
                        } else {
                            if (!currentDir.endsWith("/")) currentDir += "/";
                            currentDir += names[selected];
                        }
                        selected = 0;
                    } else {
                        if (currentDir == "/") {
                            snprintf(out, outSize, "/%s", names[selected].c_str());
                        } else {
                            snprintf(out, outSize, "%s/%s", currentDir.c_str(),
                                    names[selected].c_str());
                        }
                        return;
                    }
                    done = true;
                    continue;
                }

                if (keyboard.keyPressed(VP_KEY_BACK)) {
                    // Leave the picker and let the caller re-enter it.
                    strncpy(out, "", outSize);
                    return;
                }

                if (keyboard.keyPressed(VP_KEY_FM_UP)) {
                    selected--;
                    //delay(kDelayKeyRepeatUs / 1000);
                    lDrawPicker();
                }
                if (keyboard.keyPressed(VP_KEY_FM_DOWN)) {
                    selected++;
                    //delay(kDelayKeyRepeatUs / 1000);
                    lDrawPicker();
                }
            }
            vTaskDelay(1);
        }
    }
}