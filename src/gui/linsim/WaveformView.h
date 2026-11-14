#pragma once

#include "ColorTheme.h"
#include "SimDocument.h"

#include <QImage>
#include <QScrollBar>
#include <QWidget>

struct VisibleWaveformRow {
    int sigIndex = -1;
    int memberIndex = -1; // -1 => Scalar or Bus summary; >=0 => expanded member pin

    bool isBusMember() const { return memberIndex >= 0; }
};

class WaveformEditorWidget : public QWidget {
    Q_OBJECT

public:
    explicit WaveformEditorWidget(QWidget *parent = nullptr);

    void setDocument(SimDocument *doc);
    SimDocument *document() const { return doc_; }

    void rebuildRows();
    void refreshAll();

    // View configuration
    int cellWidth() const { return cellWidth_; }
    void setCellWidth(int w);
    int rowHeight() const { return rowHeight_; }
    void setRowHeight(int h);
    bool showGrid() const { return showGrid_; }
    void setShowGrid(bool show);
    bool wideSignals() const { return wideSignals_; }
    void setWideSignals(bool wide);

    const WaveformColorTheme &colorTheme() const { return theme_; }
    void setColorTheme(const WaveformColorTheme &theme);

    void zoomIn();
    void zoomOut();
    void zoomFit();

    // Selection & Cursor accessors
    int cursorVector() const { return cursorVec_; }
    int markerVector() const { return markerVec_; }
    int selectedSignalIndex() const;
    void selectedVectorRange(int &vStart, int &vEnd) const;
    void selectedRowRange(int &rStart, int &rEnd) const;
    void selectAll();
    void jumpToSignalAndVector(const QString &pinOrSigName, int vecIdx);

    // Find: each search starts after the current selection, wraps around once
    // and selects and scrolls to the hit.  findSignal looks at signal, bus and
    // bus member names (expanding a bus to show a member); findVectorText at
    // vector comments and $MSG texts; selectVector selects vector vecIdx.
    bool findSignal(const QString &needle, bool caseSensitive, bool wholeWords, bool backward);
    bool findVectorText(const QString &needle, bool caseSensitive, bool wholeWords, bool backward);
    bool selectVector(int vecIdx);

    // State application helpers (used by menus, shortcuts, context menus, self-test)
    void applyStateToSelection(QChar state);
    void applyStateToWholeSignal(int sigIndex, QChar state);
    void toggleBusExpanded(int sigIndex);
    void setAllBusesExpanded(bool expanded);

    // Offscreen rendering & export.  A monochrome image is black on white,
    // tells the states apart by line weight, dash and fill pattern, ignores
    // signal colours and leaves out the selection, cursor and marker.
    QImage renderToImage(int maxVectors = -1, bool monochrome = false) const;
    bool exportPng(const QString &path, QString *errorOut = nullptr) const;
    bool exportPdf(const QString &path, QString *errorOut = nullptr, bool colors = true) const;

    const QVector<VisibleWaveformRow> &visibleRows() const { return rows_; }

signals:
    void aboutToModifyDocument(const QString &description);
    void documentModified();
    void cursorChanged(const QString &statusSummary);
    void signalSelectionChanged(int sigIndex);

    // Requests handled by MainWindow dialogs
    void requestAddSignal();
    void requestRenameSignal(int sigIndex);
    void requestDeleteSignal(int sigIndex);
    void requestMoveSignal(int fromIndex, int toIndex);
    void requestSetBusValue(int sigIndex, int startVec, int endVec);
    void requestAddVectorDialog();
    void requestEditVectorAnnotation(int vecIndex);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    friend class CornerHeaderWidget;
    friend class VectorRulerWidget;
    friend class SignalHeaderWidget;
    friend class WaveformCanvasWidget;

    const WaveformColorTheme &paintTheme() const;
    QColor signalColor(const SignalItem &sig) const;

    void updateScrollBars();
    void ensureVisible(int rowIdx, int vecIdx);
    void emitCursorStatus();

    void paintCorner(QPainter &p, const QRect &rect) const;
    void paintRuler(QPainter &p, const QRect &rect, int scrollX) const;
    void paintHeaders(QPainter &p, const QRect &rect, int scrollY) const;
    void paintWaveforms(QPainter &p, const QRect &rect, int scrollX, int scrollY) const;

    void drawSingleBitCell(QPainter &p, const QRect &cellRect, QChar prevState,
                           QChar curState, const QColor &customColor) const;

    void showCanvasContextMenu(const QPoint &globalPos, int rowIdx, int vecIdx);
    void showHeaderContextMenu(const QPoint &globalPos, int rowIdx);
    void showRulerContextMenu(const QPoint &globalPos, int vecIdx);

    SimDocument *doc_ = nullptr;
    QVector<VisibleWaveformRow> rows_;
    WaveformColorTheme theme_;

    int headerWidth_ = 248;
    int rulerHeight_ = 32;
    int cellWidth_ = 36;
    int rowHeight_ = 30;
    bool showGrid_ = true;
    bool wideSignals_ = false;
    mutable bool monochrome_ = false; // set only while renderToImage paints

    int cursorVec_ = 0;
    int markerVec_ = -1;
    int selRowAnchor_ = 0;
    int selRowEnd_ = 0;
    int selVecAnchor_ = 0;
    int selVecEnd_ = 0;
    bool draggingCanvas_ = false;
    bool draggingRuler_ = false;

    QWidget *cornerWidget_ = nullptr;
    QWidget *rulerWidget_ = nullptr;
    QWidget *headerWidget_ = nullptr;
    QWidget *canvasWidget_ = nullptr;
    QScrollBar *hScroll_ = nullptr;
    QScrollBar *vScroll_ = nullptr;
};
