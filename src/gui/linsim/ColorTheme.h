#pragma once

#include <QColor>
#include <QSettings>

struct WaveformColorTheme {
    QColor canvasBg{16, 21, 30};
    QColor canvasAltRowBg{21, 27, 38};
    QColor gridLine{38, 48, 66};
    QColor gridMajorLine{55, 69, 94};
    QColor rulerBg{24, 31, 44};
    QColor rulerFg{203, 213, 225};
    QColor headerBg{22, 28, 40};
    QColor headerSelectedBg{37, 56, 88};
    QColor headerFg{226, 232, 240};
    QColor selectionFill{59, 130, 246, 55};
    QColor cursorLine{250, 204, 21};
    QColor markerLine{244, 114, 182};

    // Signal states (distinct colors for 0/1 inputs vs L/H outputs, clocks, Z, X, *, P, Bus)
    QColor state0{56, 189, 248};       // Input Low (0) - sky cyan
    QColor state1{34, 211, 238};       // Input High (1) - bright cyan
    QColor stateL{245, 158, 11};       // Output Low (L) - amber
    QColor stateH{74, 222, 128};       // Output High (H) - bright green
    QColor stateC{192, 132, 252};      // Clock 0-1-0 (C) - violet
    QColor stateK{232, 121, 249};      // Clock 1-0-1 (K) - fuchsia
    QColor stateZ{148, 163, 184};      // High-Z (Z) - mid-rail slate
    QColor stateX{100, 116, 139};      // Don't-care / Untested (X/N) - hatched slate
    QColor stateSim{45, 212, 191};     // Simulator-determined (*) - teal hatched
    QColor stateP{244, 114, 182};      // Register Preload (P) - pink
    QColor busStroke{167, 139, 250};   // Multi-bit Bus outline - lavender
    QColor busFill{76, 29, 149, 65};   // Multi-bit Bus fill
    QColor busText{241, 245, 249};     // Multi-bit Bus value text
    QColor errorBg{239, 68, 68, 95};   // Simulation mismatch (#E) fill
    QColor errorBorder{248, 113, 113}; // Simulation mismatch (#E) border

    static WaveformColorTheme darkAnalyzer() {
        return WaveformColorTheme{};
    }

    static WaveformColorTheme classicWinSimLight() {
        WaveformColorTheme t;
        t.canvasBg = QColor(255, 255, 255);
        t.canvasAltRowBg = QColor(246, 248, 252);
        t.gridLine = QColor(218, 224, 235);
        t.gridMajorLine = QColor(180, 192, 210);
        t.rulerBg = QColor(232, 236, 244);
        t.rulerFg = QColor(25, 35, 50);
        t.headerBg = QColor(238, 241, 247);
        t.headerSelectedBg = QColor(204, 224, 255);
        t.headerFg = QColor(20, 28, 40);
        t.selectionFill = QColor(37, 99, 235, 45);
        t.cursorLine = QColor(202, 138, 4);
        t.markerLine = QColor(190, 24, 93);

        t.state0 = QColor(2, 132, 199);
        t.state1 = QColor(3, 105, 161);
        t.stateL = QColor(180, 83, 9);
        t.stateH = QColor(21, 128, 61);
        t.stateC = QColor(126, 34, 206);
        t.stateK = QColor(162, 28, 175);
        t.stateZ = QColor(100, 116, 139);
        t.stateX = QColor(120, 130, 145);
        t.stateSim = QColor(13, 148, 136);
        t.stateP = QColor(219, 39, 119);
        t.busStroke = QColor(79, 70, 229);
        t.busFill = QColor(199, 210, 254, 120);
        t.busText = QColor(30, 27, 75);
        t.errorBg = QColor(239, 68, 68, 70);
        t.errorBorder = QColor(220, 38, 38);
        return t;
    }

    // Black and white printing: black traces on white, grey grid; the
    // waveform painter tells the states apart by line weight, dashes and fill
    // patterns instead of colour.
    static WaveformColorTheme monochrome() {
        WaveformColorTheme t;
        const QColor black(0, 0, 0);
        const QColor white(255, 255, 255);
        t.canvasBg = white;
        t.canvasAltRowBg = QColor(246, 246, 246);
        t.gridLine = QColor(205, 205, 205);
        t.gridMajorLine = QColor(140, 140, 140);
        t.rulerBg = white;
        t.rulerFg = black;
        t.headerBg = white;
        t.headerSelectedBg = white;
        t.headerFg = black;
        t.selectionFill = QColor(0, 0, 0, 0);
        t.cursorLine = black;
        t.markerLine = black;

        t.state0 = black;
        t.state1 = black;
        t.stateL = black;
        t.stateH = black;
        t.stateC = black;
        t.stateK = black;
        t.stateZ = black;
        t.stateX = QColor(70, 70, 70);
        t.stateSim = QColor(40, 40, 40);
        t.stateP = black;
        t.busStroke = black;
        t.busFill = QColor(255, 255, 255, 0);
        t.busText = black;
        t.errorBg = QColor(0, 0, 0, 40);
        t.errorBorder = black;
        return t;
    }

    void save(QSettings &s) const {
        s.beginGroup("Colors");
        s.setValue("canvasBg", canvasBg);
        s.setValue("canvasAltRowBg", canvasAltRowBg);
        s.setValue("gridLine", gridLine);
        s.setValue("gridMajorLine", gridMajorLine);
        s.setValue("rulerBg", rulerBg);
        s.setValue("rulerFg", rulerFg);
        s.setValue("headerBg", headerBg);
        s.setValue("headerSelectedBg", headerSelectedBg);
        s.setValue("headerFg", headerFg);
        s.setValue("state0", state0);
        s.setValue("state1", state1);
        s.setValue("stateL", stateL);
        s.setValue("stateH", stateH);
        s.setValue("stateC", stateC);
        s.setValue("stateK", stateK);
        s.setValue("stateZ", stateZ);
        s.setValue("stateX", stateX);
        s.setValue("stateSim", stateSim);
        s.setValue("stateP", stateP);
        s.setValue("busStroke", busStroke);
        s.setValue("busFill", busFill);
        s.setValue("busText", busText);
        s.setValue("errorBorder", errorBorder);
        s.endGroup();
    }

    void load(QSettings &s) {
        s.beginGroup("Colors");
        auto rd = [&](const char *k, const QColor &def) {
            QVariant v = s.value(k);
            return v.isValid() ? v.value<QColor>() : def;
        };
        canvasBg = rd("canvasBg", canvasBg);
        canvasAltRowBg = rd("canvasAltRowBg", canvasAltRowBg);
        gridLine = rd("gridLine", gridLine);
        gridMajorLine = rd("gridMajorLine", gridMajorLine);
        rulerBg = rd("rulerBg", rulerBg);
        rulerFg = rd("rulerFg", rulerFg);
        headerBg = rd("headerBg", headerBg);
        headerSelectedBg = rd("headerSelectedBg", headerSelectedBg);
        headerFg = rd("headerFg", headerFg);
        state0 = rd("state0", state0);
        state1 = rd("state1", state1);
        stateL = rd("stateL", stateL);
        stateH = rd("stateH", stateH);
        stateC = rd("stateC", stateC);
        stateK = rd("stateK", stateK);
        stateZ = rd("stateZ", stateZ);
        stateX = rd("stateX", stateX);
        stateSim = rd("stateSim", stateSim);
        stateP = rd("stateP", stateP);
        busStroke = rd("busStroke", busStroke);
        busFill = rd("busFill", busFill);
        busText = rd("busText", busText);
        errorBorder = rd("errorBorder", errorBorder);
        s.endGroup();
    }
};
