#include <Arduino.h>
#include <libssh_esp32.h>

#include "ui_shell.h"

namespace {
UiShell g_ui;
}

void setup() {
    Serial.begin(115200);
    libssh_begin();
    g_ui.begin();
}

void loop() {
    g_ui.tick();
}
