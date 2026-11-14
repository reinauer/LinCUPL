#include "WaveformView.h"

#include <QColorDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QRegularExpression>
#include <QWheelEvent>

#ifdef LINSIM_HAS_PRINTSUPPORT
#include <QPageLayout>
#include <QPageSize>
#include <QPdfWriter>
#endif

namespace {

enum class RailPos { Low, High, Mid, Undefined };

RailPos endRailOfState(QChar st) {
    switch (st.toUpper().unicode()) {
    case '0':
    case 'L':
    case 'C': // 0 -> 1 -> 0 ends Low
        return RailPos::Low;
    case '1':
    case 'H':
    case 'K': // 1 -> 0 -> 1 ends High
        return RailPos::High;
    case 'Z':
        return RailPos::Mid;
    default:
        return RailPos::Undefined;
    }
}

RailPos startRailOfState(QChar st) {
    switch (st.toUpper().unicode()) {
    case '0':
    case 'L':
    case 'C':
        return RailPos::Low;
    case '1':
    case 'H':
    case 'K':
        return RailPos::High;
    case 'Z':
        return RailPos::Mid;
    default:
        return RailPos::Undefined;
    }
}

int railY(RailPos r, int yHigh, int yMid, int yLow) {
    switch (r) {
    case RailPos::High:
        return yHigh;
    case RailPos::Low:
        return yLow;
    case RailPos::Mid:
    case RailPos::Undefined:
        return yMid;
    }
    return yMid;
}

QString radixShortName(BusRadix r) {
    switch (r) {
    case BusRadix::Hex:
        return QStringLiteral("Hex");
    case BusRadix::Binary:
        return QStringLiteral("Bin");
    case BusRadix::Octal:
        return QStringLiteral("Oct");
    case BusRadix::Decimal:
        return QStringLiteral("Dec");
    }
    return QStringLiteral("Hex");
}

// Find match: a substring, or with wholeWords a match not inside a longer
// identifier.
bool textMatches(const QString &haystack, const QString &needle, bool caseSensitive, bool wholeWords) {
    if (needle.isEmpty()) return false;
    if (!wholeWords) {
        return haystack.contains(needle, caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive);
    }
    QRegularExpression re(QStringLiteral("(?<![A-Za-z0-9_])%1(?![A-Za-z0-9_])")
                              .arg(QRegularExpression::escape(needle)),
                          caseSensitive ? QRegularExpression::NoPatternOption
                                        : QRegularExpression::CaseInsensitiveOption);
    return re.match(haystack).hasMatch();
}

} // namespace

class CornerHeaderWidget : public QWidget {
public:
    explicit CornerHeaderWidget(WaveformEditorWidget *owner) : QWidget(owner), owner_(owner) {}

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        owner_->paintCorner(p, rect());
    }

private:
    WaveformEditorWidget *owner_;
};

class VectorRulerWidget : public QWidget {
public:
    explicit VectorRulerWidget(WaveformEditorWidget *owner) : QWidget(owner), owner_(owner) {
        setMouseTracking(true);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        owner_->paintRuler(p, rect(), owner_->hScroll_->value());
    }

    void mousePressEvent(QMouseEvent *e) override {
        if (!owner_->doc_ || owner_->doc_->vectors.isEmpty()) return;
        int vec = (e->position().toPoint().x() + owner_->hScroll_->value()) / owner_->cellWidth_;
        vec = qBound(0, vec, owner_->doc_->vectors.size() - 1);

        if (e->button() == Qt::RightButton) {
            owner_->cursorVec_ = vec;
            if (vec < qMin(owner_->selVecAnchor_, owner_->selVecEnd_) ||
                vec > qMax(owner_->selVecAnchor_, owner_->selVecEnd_)) {
                owner_->selVecAnchor_ = owner_->selVecEnd_ = vec;
            }
            owner_->refreshAll();
            owner_->emitCursorStatus();
            owner_->showRulerContextMenu(e->globalPosition().toPoint(), vec);
            return;
        }
        if (e->button() == Qt::LeftButton) {
            if (e->modifiers() & Qt::AltModifier) {
                owner_->markerVec_ = vec;
            } else if (e->modifiers() & Qt::ShiftModifier) {
                owner_->markerVec_ = owner_->cursorVec_;
                owner_->cursorVec_ = vec;
                owner_->selVecEnd_ = vec;
            } else {
                owner_->cursorVec_ = vec;
                owner_->selVecAnchor_ = owner_->selVecEnd_ = vec;
                owner_->selRowAnchor_ = 0;
                owner_->selRowEnd_ = qMax(0, owner_->rows_.size() - 1);
                owner_->draggingRuler_ = true;
            }
            owner_->setFocus();
            owner_->refreshAll();
            owner_->emitCursorStatus();
        }
    }

    void mouseMoveEvent(QMouseEvent *e) override {
        if (!owner_->doc_ || owner_->doc_->vectors.isEmpty()) return;
        int vec = (e->position().toPoint().x() + owner_->hScroll_->value()) / owner_->cellWidth_;
        if (vec >= 0 && vec < owner_->doc_->vectors.size()) {
            const VectorRow &vr = owner_->doc_->vectors[vec];
            QStringList tipLines;
            tipLines.append(QStringLiteral("Vector %1").arg(vec + 1, 4, 10, QLatin1Char('0')));
            if (!vr.comment.isEmpty()) tipLines.append(QStringLiteral("/* %1 */").arg(vr.comment));
            for (const QString &m : vr.messagesBefore) tipLines.append(QStringLiteral("$MSG \"%1\"").arg(m));
            for (const SimError &se : vr.errors) {
                tipLines.append(QStringLiteral("Mismatch on %1: expected %2, actual %3")
                                    .arg(se.pinName, QString(se.expected),
                                         QString(owner_->doc_->pinState(vec, se.pinName))));
            }
            setToolTip(tipLines.join(QLatin1Char('\n')));
        }
        if (owner_->draggingRuler_) {
            vec = qBound(0, vec, owner_->doc_->vectors.size() - 1);
            owner_->cursorVec_ = vec;
            owner_->selVecEnd_ = vec;
            owner_->refreshAll();
            owner_->emitCursorStatus();
        }
    }

    void mouseReleaseEvent(QMouseEvent *) override {
        owner_->draggingRuler_ = false;
    }

private:
    WaveformEditorWidget *owner_;
};

class SignalHeaderWidget : public QWidget {
public:
    explicit SignalHeaderWidget(WaveformEditorWidget *owner) : QWidget(owner), owner_(owner) {
        setMouseTracking(true);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        owner_->paintHeaders(p, rect(), owner_->vScroll_->value());
    }

    void mousePressEvent(QMouseEvent *e) override {
        if (!owner_->doc_ || owner_->rows_.isEmpty()) return;
        int row = (e->position().toPoint().y() + owner_->vScroll_->value()) / owner_->rowHeight_;
        if (row < 0 || row >= owner_->rows_.size()) return;

        const VisibleWaveformRow &vr = owner_->rows_[row];
        const SignalItem &sig = owner_->doc_->signalsList[vr.sigIndex];

        // Clicking disclosure box [+] / [-] toggles bus expand/collapse immediately
        if (e->button() == Qt::LeftButton && !vr.isBusMember() && sig.isBus() &&
            e->position().toPoint().x() >= 6 && e->position().toPoint().x() <= 26) {
            owner_->toggleBusExpanded(vr.sigIndex);
            return;
        }

        if (e->modifiers() & Qt::ShiftModifier) {
            owner_->selRowEnd_ = row;
        } else {
            owner_->selRowAnchor_ = owner_->selRowEnd_ = row;
            if (!owner_->doc_->vectors.isEmpty()) {
                owner_->selVecAnchor_ = 0;
                owner_->selVecEnd_ = owner_->doc_->vectors.size() - 1;
            }
        }
        owner_->setFocus();
        owner_->refreshAll();
        emit owner_->signalSelectionChanged(vr.sigIndex);
        owner_->emitCursorStatus();

        if (e->button() == Qt::RightButton) {
            owner_->showHeaderContextMenu(e->globalPosition().toPoint(), row);
        }
    }

    void mouseDoubleClickEvent(QMouseEvent *e) override {
        if (!owner_->doc_ || owner_->rows_.isEmpty()) return;
        int row = (e->position().toPoint().y() + owner_->vScroll_->value()) / owner_->rowHeight_;
        if (row < 0 || row >= owner_->rows_.size()) return;
        const VisibleWaveformRow &vr = owner_->rows_[row];
        if (!vr.isBusMember() && owner_->doc_->signalsList[vr.sigIndex].isBus()) {
            owner_->toggleBusExpanded(vr.sigIndex);
        } else if (!vr.isBusMember()) {
            emit owner_->requestRenameSignal(vr.sigIndex);
        }
    }

private:
    WaveformEditorWidget *owner_;
};

class WaveformCanvasWidget : public QWidget {
public:
    explicit WaveformCanvasWidget(WaveformEditorWidget *owner) : QWidget(owner), owner_(owner) {
        setMouseTracking(true);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        owner_->paintWaveforms(p, rect(), owner_->hScroll_->value(), owner_->vScroll_->value());
    }

    void mousePressEvent(QMouseEvent *e) override {
        if (!owner_->doc_ || owner_->rows_.isEmpty() || owner_->doc_->vectors.isEmpty()) return;
        int vec = (e->position().toPoint().x() + owner_->hScroll_->value()) / owner_->cellWidth_;
        int row = (e->position().toPoint().y() + owner_->vScroll_->value()) / owner_->rowHeight_;
        vec = qBound(0, vec, owner_->doc_->vectors.size() - 1);
        row = qBound(0, row, owner_->rows_.size() - 1);

        owner_->setFocus();

        if (e->button() == Qt::RightButton) {
            int r0, r1, v0, v1;
            owner_->selectedRowRange(r0, r1);
            owner_->selectedVectorRange(v0, v1);
            if (row < r0 || row > r1 || vec < v0 || vec > v1) {
                owner_->selRowAnchor_ = owner_->selRowEnd_ = row;
                owner_->selVecAnchor_ = owner_->selVecEnd_ = vec;
            }
            owner_->cursorVec_ = vec;
            owner_->refreshAll();
            emit owner_->signalSelectionChanged(owner_->rows_[row].sigIndex);
            owner_->emitCursorStatus();
            owner_->showCanvasContextMenu(e->globalPosition().toPoint(), row, vec);
            return;
        }

        if (e->button() == Qt::MiddleButton) {
            owner_->markerVec_ = vec;
            owner_->refreshAll();
            owner_->emitCursorStatus();
            return;
        }

        if (e->button() == Qt::LeftButton) {
            // Ctrl+Left-Click sets '*' (Simulator Determined)
            if (e->modifiers() & Qt::ControlModifier) {
                owner_->selRowAnchor_ = owner_->selRowEnd_ = row;
                owner_->selVecAnchor_ = owner_->selVecEnd_ = vec;
                owner_->cursorVec_ = vec;
                owner_->applyStateToSelection(QLatin1Char('*'));
                return;
            }

            if (e->modifiers() & Qt::ShiftModifier) {
                if (owner_->markerVec_ < 0) owner_->markerVec_ = owner_->cursorVec_;
                owner_->selRowEnd_ = row;
                owner_->selVecEnd_ = vec;
                owner_->cursorVec_ = vec;
            } else {
                owner_->selRowAnchor_ = owner_->selRowEnd_ = row;
                owner_->selVecAnchor_ = owner_->selVecEnd_ = vec;
                owner_->cursorVec_ = vec;
                owner_->draggingCanvas_ = true;
            }
            owner_->refreshAll();
            emit owner_->signalSelectionChanged(owner_->rows_[row].sigIndex);
            owner_->emitCursorStatus();
        }
    }

    void mouseMoveEvent(QMouseEvent *e) override {
        if (!owner_->doc_ || owner_->rows_.isEmpty() || owner_->doc_->vectors.isEmpty()) return;
        int vec = (e->position().toPoint().x() + owner_->hScroll_->value()) / owner_->cellWidth_;
        int row = (e->position().toPoint().y() + owner_->vScroll_->value()) / owner_->rowHeight_;

        if (row >= 0 && row < owner_->rows_.size() && vec >= 0 && vec < owner_->doc_->vectors.size()) {
            const VisibleWaveformRow &vr = owner_->rows_[row];
            const SignalItem &sig = owner_->doc_->signalsList[vr.sigIndex];
            QString tip;
            if (!vr.isBusMember() && sig.isBus()) {
                tip = QStringLiteral("%1 @ Vec %2: %3 (%4)")
                          .arg(sig.displayLabel())
                          .arg(vec + 1, 4, 10, QLatin1Char('0'))
                          .arg(owner_->doc_->formatBusValue(sig, vec, sig.displayRadix),
                               owner_->doc_->busBitString(sig, vec));
            } else {
                const QString pin = vr.isBusMember() ? sig.members[vr.memberIndex] : sig.members.first();
                const QChar st = owner_->doc_->pinState(vec, pin);
                tip = QStringLiteral("%1 @ Vec %2: %3")
                          .arg(vr.isBusMember() ? pin : sig.displayLabel())
                          .arg(vec + 1, 4, 10, QLatin1Char('0'))
                          .arg(st);
                if (const SimError *se = owner_->doc_->findError(vec, pin)) {
                    tip += QStringLiteral("\nMISMATCH: Expected %1, Actual %2").arg(se->expected, st);
                }
            }
            if (!owner_->doc_->vectors[vec].comment.isEmpty()) {
                tip += QStringLiteral("\n/* %1 */").arg(owner_->doc_->vectors[vec].comment);
            }
            setToolTip(tip);
        }

        if (owner_->draggingCanvas_) {
            vec = qBound(0, vec, owner_->doc_->vectors.size() - 1);
            row = qBound(0, row, owner_->rows_.size() - 1);
            owner_->selRowEnd_ = row;
            owner_->selVecEnd_ = vec;
            owner_->cursorVec_ = vec;
            owner_->refreshAll();
            owner_->emitCursorStatus();
        }
    }

    void mouseReleaseEvent(QMouseEvent *) override {
        owner_->draggingCanvas_ = false;
    }

    void mouseDoubleClickEvent(QMouseEvent *e) override {
        if (!owner_->doc_ || owner_->rows_.isEmpty() || owner_->doc_->vectors.isEmpty()) return;
        if (e->button() != Qt::LeftButton) return;
        int vec = (e->position().toPoint().x() + owner_->hScroll_->value()) / owner_->cellWidth_;
        int row = (e->position().toPoint().y() + owner_->vScroll_->value()) / owner_->rowHeight_;
        if (row < 0 || row >= owner_->rows_.size() || vec < 0 || vec >= owner_->doc_->vectors.size()) return;

        const VisibleWaveformRow &vr = owner_->rows_[row];
        const SignalItem &sig = owner_->doc_->signalsList[vr.sigIndex];
        if (!vr.isBusMember() && sig.isBus()) {
            int v0, v1;
            owner_->selectedVectorRange(v0, v1);
            emit owner_->requestSetBusValue(vr.sigIndex, v0, v1);
        } else {
            const QString pin = vr.isBusMember() ? sig.members[vr.memberIndex] : sig.members.first();
            const QChar cur = owner_->doc_->pinState(vec, pin);
            const bool isOut = owner_->doc_->isPinOutput(pin);
            QChar next;
            if (cur == QLatin1Char('0')) next = QLatin1Char('1');
            else if (cur == QLatin1Char('1')) next = QLatin1Char('0');
            else if (cur == QLatin1Char('L')) next = QLatin1Char('H');
            else if (cur == QLatin1Char('H')) next = QLatin1Char('L');
            else if (cur == QLatin1Char('C')) next = QLatin1Char('K');
            else if (cur == QLatin1Char('K')) next = QLatin1Char('C');
            else next = isOut ? QLatin1Char('H') : QLatin1Char('1');

            owner_->selRowAnchor_ = owner_->selRowEnd_ = row;
            owner_->selVecAnchor_ = owner_->selVecEnd_ = vec;
            owner_->applyStateToSelection(next);
        }
    }

private:
    WaveformEditorWidget *owner_;
};

WaveformEditorWidget::WaveformEditorWidget(QWidget *parent)
    : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    // Small enough that tiled windows fit a 2 x 2 grid on a laptop screen;
    // the editor scrolls in both directions.
    setMinimumSize(240, 120);

    cornerWidget_ = new CornerHeaderWidget(this);
    rulerWidget_ = new VectorRulerWidget(this);
    headerWidget_ = new SignalHeaderWidget(this);
    canvasWidget_ = new WaveformCanvasWidget(this);
    hScroll_ = new QScrollBar(Qt::Horizontal, this);
    vScroll_ = new QScrollBar(Qt::Vertical, this);

    connect(hScroll_, &QScrollBar::valueChanged, this, [this]() {
        rulerWidget_->update();
        canvasWidget_->update();
    });
    connect(vScroll_, &QScrollBar::valueChanged, this, [this]() {
        headerWidget_->update();
        canvasWidget_->update();
    });
}

void WaveformEditorWidget::setDocument(SimDocument *doc) {
    doc_ = doc;
    cursorVec_ = 0;
    markerVec_ = -1;
    selRowAnchor_ = selRowEnd_ = 0;
    selVecAnchor_ = selVecEnd_ = 0;
    rebuildRows();
    emitCursorStatus();
}

void WaveformEditorWidget::rebuildRows() {
    rows_.clear();
    if (doc_) {
        for (int s = 0; s < doc_->signalsList.size(); ++s) {
            rows_.append(VisibleWaveformRow{s, -1});
            const SignalItem &sig = doc_->signalsList[s];
            if (sig.isBus() && sig.expanded) {
                for (int m = 0; m < sig.members.size(); ++m) {
                    rows_.append(VisibleWaveformRow{s, m});
                }
            }
        }
        if (!rows_.isEmpty()) {
            selRowAnchor_ = qBound(0, selRowAnchor_, rows_.size() - 1);
            selRowEnd_ = qBound(0, selRowEnd_, rows_.size() - 1);
        }
        if (!doc_->vectors.isEmpty()) {
            cursorVec_ = qBound(0, cursorVec_, doc_->vectors.size() - 1);
            selVecAnchor_ = qBound(0, selVecAnchor_, doc_->vectors.size() - 1);
            selVecEnd_ = qBound(0, selVecEnd_, doc_->vectors.size() - 1);
        }
    }
    updateScrollBars();
    refreshAll();
}

void WaveformEditorWidget::refreshAll() {
    updateScrollBars();
    cornerWidget_->update();
    rulerWidget_->update();
    headerWidget_->update();
    canvasWidget_->update();
}

void WaveformEditorWidget::setCellWidth(int w) {
    cellWidth_ = qBound(10, w, 160);
    refreshAll();
}

void WaveformEditorWidget::setRowHeight(int h) {
    rowHeight_ = qBound(20, h, 96);
    refreshAll();
}

void WaveformEditorWidget::setShowGrid(bool show) {
    showGrid_ = show;
    refreshAll();
}

void WaveformEditorWidget::setWideSignals(bool wide) {
    wideSignals_ = wide;
    rowHeight_ = wide ? 44 : 30;
    refreshAll();
}

void WaveformEditorWidget::setColorTheme(const WaveformColorTheme &theme) {
    theme_ = theme;
    refreshAll();
}

void WaveformEditorWidget::zoomIn() {
    setCellWidth(qRound(cellWidth_ * 1.25));
}

void WaveformEditorWidget::zoomOut() {
    setCellWidth(qMax(10, qRound(cellWidth_ / 1.25)));
}

void WaveformEditorWidget::zoomFit() {
    if (!doc_ || doc_->vectors.isEmpty()) return;
    int availW = qMax(120, canvasWidget_->width() - 16);
    int fitW = availW / doc_->vectors.size();
    setCellWidth(qBound(10, fitW, 96));
}

int WaveformEditorWidget::selectedSignalIndex() const {
    if (!doc_ || rows_.isEmpty()) return -1;
    int r = qBound(0, selRowEnd_, rows_.size() - 1);
    return rows_[r].sigIndex;
}

void WaveformEditorWidget::selectedVectorRange(int &vStart, int &vEnd) const {
    if (!doc_ || doc_->vectors.isEmpty()) {
        vStart = vEnd = 0;
        return;
    }
    vStart = qBound(0, qMin(selVecAnchor_, selVecEnd_), doc_->vectors.size() - 1);
    vEnd = qBound(0, qMax(selVecAnchor_, selVecEnd_), doc_->vectors.size() - 1);
}

void WaveformEditorWidget::selectedRowRange(int &rStart, int &rEnd) const {
    if (rows_.isEmpty()) {
        rStart = rEnd = 0;
        return;
    }
    rStart = qBound(0, qMin(selRowAnchor_, selRowEnd_), rows_.size() - 1);
    rEnd = qBound(0, qMax(selRowAnchor_, selRowEnd_), rows_.size() - 1);
}

void WaveformEditorWidget::selectAll() {
    if (!doc_ || rows_.isEmpty() || doc_->vectors.isEmpty()) return;
    selRowAnchor_ = 0;
    selRowEnd_ = rows_.size() - 1;
    selVecAnchor_ = 0;
    selVecEnd_ = doc_->vectors.size() - 1;
    refreshAll();
    emitCursorStatus();
}

void WaveformEditorWidget::jumpToSignalAndVector(const QString &pinOrSigName, int vecIdx) {
    if (!doc_) return;
    int targetRow = 0;
    for (int r = 0; r < rows_.size(); ++r) {
        const VisibleWaveformRow &vr = rows_[r];
        const SignalItem &sig = doc_->signalsList[vr.sigIndex];
        if (vr.isBusMember()) {
            if (sig.members[vr.memberIndex].compare(pinOrSigName, Qt::CaseInsensitive) == 0) {
                targetRow = r;
                break;
            }
        } else {
            if (sig.name.compare(pinOrSigName, Qt::CaseInsensitive) == 0 ||
                sig.members.contains(pinOrSigName, Qt::CaseInsensitive)) {
                targetRow = r;
                break;
            }
        }
    }
    if (!doc_->vectors.isEmpty()) {
        cursorVec_ = qBound(0, vecIdx, doc_->vectors.size() - 1);
        selVecAnchor_ = selVecEnd_ = cursorVec_;
    }
    selRowAnchor_ = selRowEnd_ = targetRow;
    ensureVisible(targetRow, cursorVec_);
    refreshAll();
    emit signalSelectionChanged(rows_.isEmpty() ? -1 : rows_[targetRow].sigIndex);
    emitCursorStatus();
}

bool WaveformEditorWidget::findSignal(const QString &needle, bool caseSensitive, bool wholeWords, bool backward) {
    if (!doc_ || doc_->signalsList.isEmpty() || needle.isEmpty()) return false;

    // Every name in display order: each signal or bus, then the bus members,
    // whether the bus is expanded or not.
    QVector<VisibleWaveformRow> names;
    for (int s = 0; s < doc_->signalsList.size(); ++s) {
        names.append(VisibleWaveformRow{s, -1});
        const SignalItem &sig = doc_->signalsList[s];
        if (sig.isBus()) {
            for (int m = 0; m < sig.members.size(); ++m) {
                names.append(VisibleWaveformRow{s, m});
            }
        }
    }
    auto matches = [&](const VisibleWaveformRow &n) {
        const SignalItem &sig = doc_->signalsList[n.sigIndex];
        if (n.isBusMember()) {
            return textMatches(sig.members[n.memberIndex], needle, caseSensitive, wholeWords);
        }
        return textMatches(sig.displayLabel(), needle, caseSensitive, wholeWords) ||
               textMatches(sig.name, needle, caseSensitive, wholeWords);
    };

    const int n = names.size();
    int cur = backward ? 0 : n - 1;
    if (!rows_.isEmpty()) {
        const VisibleWaveformRow &sel = rows_[qBound(0, selRowEnd_, rows_.size() - 1)];
        for (int i = 0; i < n; ++i) {
            if (names[i].sigIndex == sel.sigIndex && names[i].memberIndex == sel.memberIndex) {
                cur = i;
                break;
            }
        }
    }

    for (int i = 1; i <= n; ++i) {
        const VisibleWaveformRow hit = names[((cur + (backward ? -i : i)) % n + n) % n];
        if (!matches(hit)) continue;

        SignalItem &sig = doc_->signalsList[hit.sigIndex];
        if (hit.isBusMember() && !sig.expanded) {
            sig.expanded = true;
            rebuildRows();
        }
        int row = 0;
        for (int r = 0; r < rows_.size(); ++r) {
            if (rows_[r].sigIndex == hit.sigIndex && rows_[r].memberIndex == hit.memberIndex) {
                row = r;
                break;
            }
        }
        // Select the whole row, as a click on the signal name does.
        selRowAnchor_ = selRowEnd_ = row;
        if (!doc_->vectors.isEmpty()) {
            selVecAnchor_ = 0;
            selVecEnd_ = doc_->vectors.size() - 1;
        }
        ensureVisible(row, cursorVec_);
        refreshAll();
        emit signalSelectionChanged(hit.sigIndex);
        emitCursorStatus();
        return true;
    }
    return false;
}

bool WaveformEditorWidget::findVectorText(const QString &needle, bool caseSensitive, bool wholeWords, bool backward) {
    if (!doc_ || doc_->vectors.isEmpty() || needle.isEmpty()) return false;
    const int n = doc_->vectors.size();
    const int cur = qBound(0, cursorVec_, n - 1);
    for (int i = 1; i <= n; ++i) {
        const int v = ((cur + (backward ? -i : i)) % n + n) % n;
        const VectorRow &vr = doc_->vectors[v];
        bool hit = textMatches(vr.comment, needle, caseSensitive, wholeWords);
        for (int m = 0; !hit && m < vr.messagesBefore.size(); ++m) {
            hit = textMatches(vr.messagesBefore[m], needle, caseSensitive, wholeWords);
        }
        if (hit) return selectVector(v);
    }
    return false;
}

bool WaveformEditorWidget::selectVector(int vecIdx) {
    if (!doc_ || vecIdx < 0 || vecIdx >= doc_->vectors.size()) return false;
    // Select the whole vector column, as a click on the ruler does.
    cursorVec_ = vecIdx;
    selVecAnchor_ = selVecEnd_ = vecIdx;
    if (!rows_.isEmpty()) {
        selRowAnchor_ = 0;
        selRowEnd_ = rows_.size() - 1;
    }
    const int x0 = vecIdx * cellWidth_;
    if (x0 < hScroll_->value() || x0 + cellWidth_ > hScroll_->value() + canvasWidget_->width()) {
        hScroll_->setValue(x0 - qMax(0, (canvasWidget_->width() - cellWidth_) / 2));
    }
    refreshAll();
    emitCursorStatus();
    return true;
}

void WaveformEditorWidget::applyStateToSelection(QChar state) {
    if (!doc_ || rows_.isEmpty() || doc_->vectors.isEmpty()) return;
    if (!SimDocument::isValidStateChar(state)) return;

    int r0, r1, v0, v1;
    selectedRowRange(r0, r1);
    selectedVectorRange(v0, v1);

    emit aboutToModifyDocument(QStringLiteral("Set state %1").arg(state.toUpper()));

    for (int r = r0; r <= r1; ++r) {
        const VisibleWaveformRow &vr = rows_[r];
        const SignalItem &sig = doc_->signalsList[vr.sigIndex];
        for (int v = v0; v <= v1; ++v) {
            if (vr.isBusMember()) {
                doc_->setPinState(v, sig.members[vr.memberIndex], state);
            } else {
                for (const QString &m : sig.members) {
                    doc_->setPinState(v, m, state);
                }
            }
        }
    }

    refreshAll();
    emit documentModified();
    emitCursorStatus();
}

void WaveformEditorWidget::applyStateToWholeSignal(int sigIndex, QChar state) {
    if (!doc_ || sigIndex < 0 || sigIndex >= doc_->signalsList.size()) return;
    if (doc_->vectors.isEmpty() || !SimDocument::isValidStateChar(state)) return;

    emit aboutToModifyDocument(QStringLiteral("Set whole signal %1 to %2")
                                   .arg(doc_->signalsList[sigIndex].displayLabel(), QString(state.toUpper())));

    const SignalItem &sig = doc_->signalsList[sigIndex];
    for (int v = 0; v < doc_->vectors.size(); ++v) {
        for (const QString &m : sig.members) {
            doc_->setPinState(v, m, state);
        }
    }
    refreshAll();
    emit documentModified();
    emitCursorStatus();
}

void WaveformEditorWidget::toggleBusExpanded(int sigIndex) {
    if (!doc_ || sigIndex < 0 || sigIndex >= doc_->signalsList.size()) return;
    SignalItem &sig = doc_->signalsList[sigIndex];
    if (!sig.isBus()) return;
    sig.expanded = !sig.expanded;
    rebuildRows();
}

void WaveformEditorWidget::setAllBusesExpanded(bool expanded) {
    if (!doc_) return;
    for (SignalItem &sig : doc_->signalsList) {
        if (sig.isBus()) sig.expanded = expanded;
    }
    rebuildRows();
}

void WaveformEditorWidget::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    const int sbW = vScroll_->sizeHint().width();
    const int sbH = hScroll_->sizeHint().height();
    const int contentW = qMax(0, width() - headerWidth_ - sbW);
    const int contentH = qMax(0, height() - rulerHeight_ - sbH);

    cornerWidget_->setGeometry(0, 0, headerWidth_, rulerHeight_);
    rulerWidget_->setGeometry(headerWidth_, 0, contentW, rulerHeight_);
    headerWidget_->setGeometry(0, rulerHeight_, headerWidth_, contentH);
    canvasWidget_->setGeometry(headerWidth_, rulerHeight_, contentW, contentH);
    vScroll_->setGeometry(headerWidth_ + contentW, rulerHeight_, sbW, contentH);
    hScroll_->setGeometry(headerWidth_, rulerHeight_ + contentH, contentW, sbH);

    updateScrollBars();
}

void WaveformEditorWidget::updateScrollBars() {
    const int numVecs = doc_ ? doc_->vectors.size() : 0;
    const int totalW = numVecs * cellWidth_;
    const int totalH = rows_.size() * rowHeight_;

    const int viewW = qMax(1, canvasWidget_->width());
    const int viewH = qMax(1, canvasWidget_->height());

    hScroll_->setRange(0, qMax(0, totalW - viewW));
    hScroll_->setPageStep(viewW);
    hScroll_->setSingleStep(cellWidth_);

    vScroll_->setRange(0, qMax(0, totalH - viewH));
    vScroll_->setPageStep(viewH);
    vScroll_->setSingleStep(rowHeight_);
}

void WaveformEditorWidget::ensureVisible(int rowIdx, int vecIdx) {
    const int x0 = vecIdx * cellWidth_;
    const int x1 = x0 + cellWidth_;
    if (x0 < hScroll_->value()) {
        hScroll_->setValue(x0);
    } else if (x1 > hScroll_->value() + canvasWidget_->width()) {
        hScroll_->setValue(x1 - canvasWidget_->width());
    }

    const int y0 = rowIdx * rowHeight_;
    const int y1 = y0 + rowHeight_;
    if (y0 < vScroll_->value()) {
        vScroll_->setValue(y0);
    } else if (y1 > vScroll_->value() + canvasWidget_->height()) {
        vScroll_->setValue(y1 - canvasWidget_->height());
    }
}

void WaveformEditorWidget::emitCursorStatus() {
    if (!doc_ || doc_->vectors.isEmpty() || rows_.isEmpty()) {
        emit cursorChanged(QStringLiteral("No vectors loaded"));
        return;
    }

    const int v = qBound(0, cursorVec_, doc_->vectors.size() - 1);
    const int r = qBound(0, selRowEnd_, rows_.size() - 1);
    const VisibleWaveformRow &vr = rows_[r];
    const SignalItem &sig = doc_->signalsList[vr.sigIndex];

    QStringList parts;
    parts.append(QStringLiteral("Vec: %1/%2")
                     .arg(v + 1, 4, 10, QLatin1Char('0'))
                     .arg(doc_->vectors.size(), 4, 10, QLatin1Char('0')));

    if (markerVec_ >= 0 && markerVec_ < doc_->vectors.size()) {
        int delta = v - markerVec_;
        parts.append(QStringLiteral("Ref: %1 (Δ %2%3)")
                         .arg(markerVec_ + 1, 4, 10, QLatin1Char('0'))
                         .arg(delta >= 0 ? QStringLiteral("+") : QString())
                         .arg(delta));
    }

    int sv0, sv1;
    selectedVectorRange(sv0, sv1);
    if (sv1 > sv0) {
        parts.append(QStringLiteral("Sel: %1..%2 (%3 vecs)").arg(sv0 + 1).arg(sv1 + 1).arg(sv1 - sv0 + 1));
    }

    if (!vr.isBusMember() && sig.isBus()) {
        parts.append(QStringLiteral("%1 [%2b] = %3 (%4)")
                         .arg(sig.displayLabel())
                         .arg(sig.width())
                         .arg(doc_->formatBusValue(sig, v, BusRadix::Hex),
                              doc_->busBitString(sig, v)));
    } else {
        const QString pin = vr.isBusMember() ? sig.members[vr.memberIndex] : sig.members.first();
        const QChar st = doc_->pinState(v, pin);
        parts.append(QStringLiteral("%1 = %2").arg(vr.isBusMember() ? pin : sig.displayLabel(), QString(st)));
        if (const SimError *se = doc_->findError(v, pin)) {
            parts.append(QStringLiteral("MISMATCH(Expected: %1, Actual: %2)").arg(se->expected, st));
        }
    }

    if (!doc_->vectors[v].comment.isEmpty()) {
        parts.append(QStringLiteral("/* %1 */").arg(doc_->vectors[v].comment));
    }

    emit cursorChanged(parts.join(QStringLiteral("  |  ")));
}

void WaveformEditorWidget::keyPressEvent(QKeyEvent *event) {
    if (!doc_ || rows_.isEmpty() || doc_->vectors.isEmpty()) {
        QWidget::keyPressEvent(event);
        return;
    }

    const bool shift = (event->modifiers() & Qt::ShiftModifier) != 0;

    switch (event->key()) {
    case Qt::Key_Left:
        cursorVec_ = qMax(0, cursorVec_ - 1);
        selVecEnd_ = cursorVec_;
        if (!shift) selVecAnchor_ = cursorVec_;
        ensureVisible(selRowEnd_, cursorVec_);
        refreshAll();
        emitCursorStatus();
        return;
    case Qt::Key_Right:
        cursorVec_ = qMin(doc_->vectors.size() - 1, cursorVec_ + 1);
        selVecEnd_ = cursorVec_;
        if (!shift) selVecAnchor_ = cursorVec_;
        ensureVisible(selRowEnd_, cursorVec_);
        refreshAll();
        emitCursorStatus();
        return;
    case Qt::Key_Up:
        selRowEnd_ = qMax(0, selRowEnd_ - 1);
        if (!shift) selRowAnchor_ = selRowEnd_;
        ensureVisible(selRowEnd_, cursorVec_);
        refreshAll();
        emit signalSelectionChanged(rows_[selRowEnd_].sigIndex);
        emitCursorStatus();
        return;
    case Qt::Key_Down:
        selRowEnd_ = qMin(rows_.size() - 1, selRowEnd_ + 1);
        if (!shift) selRowAnchor_ = selRowEnd_;
        ensureVisible(selRowEnd_, cursorVec_);
        refreshAll();
        emit signalSelectionChanged(rows_[selRowEnd_].sigIndex);
        emitCursorStatus();
        return;
    case Qt::Key_Home:
        cursorVec_ = 0;
        selVecEnd_ = 0;
        if (!shift) selVecAnchor_ = 0;
        ensureVisible(selRowEnd_, cursorVec_);
        refreshAll();
        emitCursorStatus();
        return;
    case Qt::Key_End:
        cursorVec_ = doc_->vectors.size() - 1;
        selVecEnd_ = cursorVec_;
        if (!shift) selVecAnchor_ = cursorVec_;
        ensureVisible(selRowEnd_, cursorVec_);
        refreshAll();
        emitCursorStatus();
        return;
    case Qt::Key_Space: {
        const VisibleWaveformRow &vr = rows_[selRowEnd_];
        const SignalItem &sig = doc_->signalsList[vr.sigIndex];
        if (!vr.isBusMember() && sig.isBus()) {
            toggleBusExpanded(vr.sigIndex);
        } else {
            const QString pin = vr.isBusMember() ? sig.members[vr.memberIndex] : sig.members.first();
            const QChar cur = doc_->pinState(cursorVec_, pin);
            const bool isOut = doc_->isPinOutput(pin);
            QChar next = (cur == QLatin1Char('0'))   ? QLatin1Char('1')
                         : (cur == QLatin1Char('1')) ? QLatin1Char('0')
                         : (cur == QLatin1Char('L')) ? QLatin1Char('H')
                         : (cur == QLatin1Char('H')) ? QLatin1Char('L')
                                                     : (isOut ? QLatin1Char('H') : QLatin1Char('1'));
            applyStateToSelection(next);
        }
        return;
    }
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_B: {
        const VisibleWaveformRow &vr = rows_[selRowEnd_];
        if (!vr.isBusMember() && doc_->signalsList[vr.sigIndex].isBus()) {
            int v0, v1;
            selectedVectorRange(v0, v1);
            emit requestSetBusValue(vr.sigIndex, v0, v1);
            return;
        }
        break;
    }
    default:
        break;
    }

    // Direct single-key state entry: 0, 1, C, K, P, L, H, Z, X, *, N
    const QString text = event->text();
    if (text.size() == 1 && !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
        QChar ch = text[0].toUpper();
        if (SimDocument::isValidStateChar(ch)) {
            applyStateToSelection(ch);
            return;
        }
    }

    QWidget::keyPressEvent(event);
}

void WaveformEditorWidget::wheelEvent(QWheelEvent *event) {
    if (event->modifiers() & Qt::ControlModifier) {
        if (event->angleDelta().y() > 0) zoomIn();
        else if (event->angleDelta().y() < 0) zoomOut();
        event->accept();
        return;
    }
    if (event->modifiers() & Qt::ShiftModifier || qAbs(event->angleDelta().x()) > qAbs(event->angleDelta().y())) {
        int delta = (event->angleDelta().x() != 0) ? event->angleDelta().x() : event->angleDelta().y();
        hScroll_->setValue(hScroll_->value() - delta / 2);
        event->accept();
        return;
    }
    vScroll_->setValue(vScroll_->value() - event->angleDelta().y() / 2);
    event->accept();
}

void WaveformEditorWidget::paintCorner(QPainter &p, const QRect &rect) const {
    const WaveformColorTheme &th = paintTheme();
    p.fillRect(rect, th.rulerBg);
    p.setPen(th.gridMajorLine);
    p.drawLine(rect.bottomLeft(), rect.bottomRight());
    p.drawLine(rect.topRight(), rect.bottomRight());

    QFont f = font();
    f.setBold(true);
    f.setPointSizeF(qMax(9.0, f.pointSizeF() - 0.5));
    p.setFont(f);
    p.setPen(th.rulerFg);

    const int nSigs = doc_ ? doc_->signalsList.size() : 0;
    const int nVecs = doc_ ? doc_->vectors.size() : 0;
    p.drawText(rect.adjusted(10, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft,
               QStringLiteral("Signal (%1)  •  Vecs (%2)").arg(nSigs).arg(nVecs));
}

void WaveformEditorWidget::paintRuler(QPainter &p, const QRect &rect, int scrollX) const {
    const WaveformColorTheme &th = paintTheme();
    // A black-and-white print leaves out the cursor and the reference marker.
    const int cursorVec = monochrome_ ? -1 : cursorVec_;
    const int markerVec = monochrome_ ? -1 : markerVec_;
    p.fillRect(rect, th.rulerBg);
    p.setPen(th.gridMajorLine);
    p.drawLine(rect.bottomLeft(), rect.bottomRight());

    if (!doc_ || doc_->vectors.isEmpty()) return;

    const int nVecs = doc_->vectors.size();
    const int vStart = qMax(0, scrollX / cellWidth_);
    const int vEnd = qMin(nVecs - 1, (scrollX + rect.width()) / cellWidth_ + 1);

    int labelStep = 1;
    if (cellWidth_ < 16) labelStep = 10;
    else if (cellWidth_ < 26) labelStep = 5;
    else if (cellWidth_ < 42) labelStep = 2;

    int sel0, sel1;
    selectedVectorRange(sel0, sel1);

    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    f.setPointSizeF(9.5);
    p.setFont(f);

    for (int v = vStart; v <= vEnd; ++v) {
        const int x = v * cellWidth_ - scrollX;
        const QRect cellRect(x, 0, cellWidth_, rect.height());

        if (v >= sel0 && v <= sel1) {
            p.fillRect(cellRect, th.selectionFill);
        }
        if (!doc_->vectors[v].errors.isEmpty()) {
            p.fillRect(QRect(x, rect.height() - 5, cellWidth_, 5), th.errorBorder);
        } else if (!doc_->vectors[v].messagesBefore.isEmpty() || !doc_->vectors[v].comment.isEmpty()) {
            QPolygonF tri;
            tri << QPointF(x + cellWidth_ - 7, 2)
                << QPointF(x + cellWidth_ - 1, 2)
                << QPointF(x + cellWidth_ - 1, 8);
            p.setPen(Qt::NoPen);
            p.setBrush(th.cursorLine);
            p.drawPolygon(tri);
        }

        const bool isMajor = ((v + 1) % 5 == 0) || (v == 0);
        p.setPen(isMajor ? th.gridMajorLine : th.gridLine);
        p.drawLine(x, isMajor ? 4 : rect.height() - 10, x, rect.height() - 1);

        if (v == cursorVec) {
            p.fillRect(QRect(x, rect.height() - 3, cellWidth_, 3), th.cursorLine);
        }
        if (v == markerVec) {
            p.fillRect(QRect(x, 0, cellWidth_, 3), th.markerLine);
        }

        if ((v % labelStep == 0) || (v == cursorVec && cellWidth_ >= 34)) {
            p.setPen(v == cursorVec ? th.cursorLine : th.rulerFg);
            const QString lbl = (cellWidth_ >= 36)
                                    ? QStringLiteral("%1").arg(v + 1, 4, 10, QLatin1Char('0'))
                                    : QString::number(v + 1);
            p.drawText(QRect(x + 2, 1, qMax(cellWidth_ * labelStep - 4, cellWidth_ - 2), rect.height() - 4),
                       Qt::AlignLeft | Qt::AlignVCenter, lbl);
        }
    }
}

void WaveformEditorWidget::paintHeaders(QPainter &p, const QRect &rect, int scrollY) const {
    const WaveformColorTheme &th = paintTheme();
    p.fillRect(rect, th.headerBg);
    p.setPen(th.gridMajorLine);
    p.drawLine(rect.topRight(), rect.bottomRight());

    if (!doc_ || rows_.isEmpty()) return;

    const int rStart = qMax(0, scrollY / rowHeight_);
    const int rEnd = qMin(rows_.size() - 1, (scrollY + rect.height()) / rowHeight_ + 1);

    int selR0, selR1;
    selectedRowRange(selR0, selR1);

    QFont nameFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    nameFont.setPointSizeF(10.5);

    QFont badgeFont = font();
    badgeFont.setPointSizeF(8.5);
    badgeFont.setBold(true);

    const int curV = (!doc_->vectors.isEmpty()) ? qBound(0, cursorVec_, doc_->vectors.size() - 1) : -1;

    for (int r = rStart; r <= rEnd; ++r) {
        const int y = r * rowHeight_ - scrollY;
        const QRect rowRect(0, y, rect.width() - 1, rowHeight_);
        const VisibleWaveformRow &vr = rows_[r];
        const SignalItem &sig = doc_->signalsList[vr.sigIndex];

        if (r >= selR0 && r <= selR1) {
            p.fillRect(rowRect, th.headerSelectedBg);
        } else if (r % 2 == 1) {
            p.fillRect(rowRect, th.headerBg.lighter(106));
        }

        // Optional custom color indicator bar on left edge
        if (signalColor(sig).isValid()) {
            p.fillRect(QRect(0, y + 2, 4, rowHeight_ - 4), signalColor(sig));
        }

        p.setPen(th.gridLine);
        p.drawLine(0, y + rowHeight_ - 1, rect.width() - 1, y + rowHeight_ - 1);

        int xCursor = 8;

        if (!vr.isBusMember() && sig.isBus()) {
            // Disclosure [+] / [-] button
            QRect box(8, y + (rowHeight_ - 14) / 2, 14, 14);
            p.setPen(th.busStroke);
            p.setBrush(th.canvasBg);
            p.drawRoundedRect(box, 2, 2);
            p.drawLine(box.left() + 3, box.center().y(), box.right() - 3, box.center().y());
            if (!sig.expanded) {
                p.drawLine(box.center().x(), box.top() + 3, box.center().x(), box.bottom() - 3);
            }
            xCursor = 28;
        } else if (vr.isBusMember()) {
            // Tree branch line
            p.setPen(th.gridMajorLine);
            int midX = 15;
            bool isLast = (vr.memberIndex == sig.members.size() - 1);
            p.drawLine(midX, y, midX, isLast ? (y + rowHeight_ / 2) : (y + rowHeight_));
            p.drawLine(midX, y + rowHeight_ / 2, 24, y + rowHeight_ / 2);
            xCursor = 28;
        } else {
            xCursor = 12;
        }

        // Live value at cursor on the right side of header row
        QString valText;
        QColor valColor = th.headerFg;
        if (curV >= 0) {
            if (!vr.isBusMember() && sig.isBus()) {
                valText = doc_->formatBusValue(sig, curV, sig.displayRadix);
                valColor = signalColor(sig).isValid() ? signalColor(sig) : th.busStroke;
                if (doc_->hasErrorInSignal(curV, sig)) valColor = th.errorBorder;
            } else {
                const QString pin = vr.isBusMember() ? sig.members[vr.memberIndex] : sig.members.first();
                const QChar st = doc_->pinState(curV, pin);
                valText = QString(st);
                if (doc_->findError(curV, pin)) valColor = th.errorBorder;
                else if (st == QLatin1Char('0') || st == QLatin1Char('1')) valColor = th.state1;
                else if (st == QLatin1Char('L') || st == QLatin1Char('H')) valColor = th.stateH;
                else if (st == QLatin1Char('C') || st == QLatin1Char('K')) valColor = th.stateC;
                else if (st == QLatin1Char('Z')) valColor = th.stateZ;
                else if (st == QLatin1Char('*')) valColor = th.stateSim;
            }
        }

        const int valWidth = 58;
        if (!valText.isEmpty()) {
            p.setFont(nameFont);
            p.setPen(valColor);
            QFontMetrics fm(nameFont);
            const QString elidedVal = fm.elidedText(valText, Qt::ElideRight, valWidth - 4);
            p.drawText(QRect(rect.width() - valWidth - 4, y, valWidth, rowHeight_),
                       Qt::AlignRight | Qt::AlignVCenter, elidedVal);
        }

        // Direction / width badge
        p.setFont(badgeFont);
        QString badgeText;
        QColor badgeCol;
        if (!vr.isBusMember() && sig.isBus()) {
            badgeText = QStringLiteral("%1b·%2").arg(sig.width()).arg(radixShortName(sig.displayRadix));
            badgeCol = th.busStroke;
        } else {
            const QString pin = vr.isBusMember() ? sig.members[vr.memberIndex] : sig.members.first();
            auto pmit = doc_->pinMeta.find(pin.toLower());
            const bool isOut = doc_->isPinOutput(pin);
            if (pmit != doc_->pinMeta.end() && !pmit->pinNumber.isEmpty()) {
                badgeText = QStringLiteral("p%1").arg(pmit->pinNumber);
            } else {
                badgeText = isOut ? QStringLiteral("OUT") : QStringLiteral("IN");
            }
            badgeCol = isOut ? th.stateH : th.state0;
        }

        QFontMetrics bfm(badgeFont);
        const int bW = bfm.horizontalAdvance(badgeText) + 8;
        const QRect badgeRect(rect.width() - valWidth - bW - 8, y + (rowHeight_ - 16) / 2, bW, 16);
        if (badgeRect.left() > xCursor + 40) {
            QColor bg = badgeCol;
            bg.setAlpha(38);
            p.setPen(Qt::NoPen);
            p.setBrush(bg);
            p.drawRoundedRect(badgeRect, 3, 3);
            p.setPen(badgeCol);
            p.drawText(badgeRect, Qt::AlignCenter, badgeText);
        }

        // Signal or Member Name
        QFont nf = nameFont;
        nf.setBold(!vr.isBusMember() && sig.isBus());
        p.setFont(nf);
        p.setPen(vr.isBusMember() ? th.headerFg.darker(112) : th.headerFg);
        const QString dispName = vr.isBusMember() ? sig.members[vr.memberIndex] : sig.displayLabel();
        const int availNameW = qMax(24, badgeRect.left() - xCursor - 6);
        QFontMetrics nfm(nf);
        p.drawText(QRect(xCursor, y, availNameW, rowHeight_),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   nfm.elidedText(dispName, Qt::ElideMiddle, availNameW));
    }
}

void WaveformEditorWidget::drawSingleBitCell(QPainter &p, const QRect &cellRect,
                                             QChar prevState, QChar curState,
                                             const QColor &customColor) const {
    const WaveformColorTheme &th = paintTheme();
    const int padY = qMax(5, rowHeight_ / 5);
    const int yHigh = cellRect.top() + padY;
    const int yLow = cellRect.bottom() - padY;
    const int yMid = (yHigh + yLow) / 2;
    const int x0 = cellRect.left();
    const int x1 = cellRect.right() + 1;
    const int w = cellRect.width();

    const QChar st = curState.toUpper();
    const RailPos prevEnd = endRailOfState(prevState);
    const RailPos curStart = startRailOfState(st);

    auto pickColor = [&](const QColor &themeCol) {
        return customColor.isValid() ? customColor : themeCol;
    };

    // Draw vertical transition edge at x0 when both previous and current states have defined logic rails
    auto drawTransitionEdge = [&](const QColor &col) {
        if (prevEnd != RailPos::Undefined && curStart != RailPos::Undefined && prevEnd != curStart) {
            QPen edgePen(col, 1.8);
            p.setPen(edgePen);
            p.drawLine(x0, railY(prevEnd, yHigh, yMid, yLow), x0, railY(curStart, yHigh, yMid, yLow));
        }
    };

    if (st == QLatin1Char('0') || st == QLatin1Char('1') ||
        st == QLatin1Char('L') || st == QLatin1Char('H')) {
        const bool isHigh = (st == QLatin1Char('1') || st == QLatin1Char('H'));
        const bool isOut = (st == QLatin1Char('L') || st == QLatin1Char('H'));
        const QColor col = pickColor(isOut ? (isHigh ? th.stateH : th.stateL)
                                           : (isHigh ? th.state1 : th.state0));
        drawTransitionEdge(col);
        // Without colours, tested outputs (L/H) stand out from driven inputs
        // (0/1) by a heavier line and their letter.
        QPen pen(col, monochrome_ ? (isOut ? 2.8 : 1.3) : (isOut ? 2.2 : 1.9));
        p.setPen(pen);
        const int y = isHigh ? yHigh : yLow;
        p.drawLine(x0, y, x1, y);

        if (isOut && w >= (monochrome_ ? 14 : 26)) {
            QFont tf = p.font();
            tf.setPointSizeF(7.5);
            p.setFont(tf);
            QColor subCol = col;
            subCol.setAlpha(175);
            p.setPen(subCol);
            p.drawText(cellRect.adjusted(2, 0, -2, 0),
                       Qt::AlignCenter, QString(st));
        }
        return;
    }

    if (st == QLatin1Char('C') || st == QLatin1Char('K')) {
        const bool isC = (st == QLatin1Char('C'));
        const QColor col = pickColor(isC ? th.stateC : th.stateK);
        drawTransitionEdge(col);

        const int xRiseOrFall = x0 + qMax(3, w / 4);
        const int xReturn = x0 + qMax(6, (w * 3) / 4);
        const int yBase = isC ? yLow : yHigh;
        const int yPeak = isC ? yHigh : yLow;

        QPen pen(col, 1.9);
        p.setPen(pen);
        p.drawLine(x0, yBase, xRiseOrFall, yBase);
        p.drawLine(xRiseOrFall, yBase, xRiseOrFall, yPeak);
        p.drawLine(xRiseOrFall, yPeak, xReturn, yPeak);
        p.drawLine(xReturn, yPeak, xReturn, yBase);
        p.drawLine(xReturn, yBase, x1, yBase);

        // Draw active-edge arrowhead on xRiseOrFall
        if (w >= 14 && (yLow - yHigh) >= 10) {
            const int midEdgeY = (yHigh + yLow) / 2;
            QPolygonF arrow;
            if (isC) {
                arrow << QPointF(xRiseOrFall, midEdgeY - 3)
                      << QPointF(xRiseOrFall - 3.2, midEdgeY + 2.5)
                      << QPointF(xRiseOrFall + 3.2, midEdgeY + 2.5);
            } else {
                arrow << QPointF(xRiseOrFall, midEdgeY + 3)
                      << QPointF(xRiseOrFall - 3.2, midEdgeY - 2.5)
                      << QPointF(xRiseOrFall + 3.2, midEdgeY - 2.5);
            }
            p.setPen(Qt::NoPen);
            p.setBrush(col);
            p.drawPolygon(arrow);
            p.setBrush(Qt::NoBrush);
        }
        return;
    }

    if (st == QLatin1Char('Z')) {
        const QColor col = pickColor(th.stateZ);
        drawTransitionEdge(col);
        QPen pen(col, 1.8, Qt::DashLine);
        p.setPen(pen);
        p.drawLine(x0, yMid, x1, yMid);
        if (w >= 20) {
            QFont tf = p.font();
            tf.setPointSizeF(7.5);
            p.setFont(tf);
            p.setPen(col);
            p.drawText(QRect(x0, yHigh - 2, w, yMid - yHigh + 2), Qt::AlignCenter, QStringLiteral("Z"));
        }
        return;
    }

    if (st == QLatin1Char('*')) {
        const QColor col = pickColor(th.stateSim);
        QRect band(x0, yHigh, w, yLow - yHigh);
        QColor fill = col;
        fill.setAlpha(45);
        p.fillRect(band, QBrush(fill, Qt::FDiagPattern));
        p.setPen(QPen(col, 1.4, Qt::DotLine));
        p.drawLine(x0, yHigh, x1, yHigh);
        p.drawLine(x0, yLow, x1, yLow);
        if (w >= 14) {
            QFont tf = p.font();
            tf.setPointSizeF(8.5);
            tf.setBold(true);
            p.setFont(tf);
            p.setPen(col);
            p.drawText(band, Qt::AlignCenter, QStringLiteral("*"));
        }
        return;
    }

    if (st == QLatin1Char('P')) {
        const QColor col = pickColor(th.stateP);
        QRect box(x0 + 2, yHigh, qMax(6, w - 4), yLow - yHigh);
        QColor fill = col;
        fill.setAlpha(55);
        p.setPen(QPen(col, 1.6));
        p.setBrush(fill);
        p.drawRoundedRect(box, 3, 3);
        p.setBrush(Qt::NoBrush);
        if (w >= 14) {
            QFont tf = p.font();
            tf.setPointSizeF(8.0);
            tf.setBold(true);
            p.setFont(tf);
            p.setPen(col);
            p.drawText(box, Qt::AlignCenter, QStringLiteral("P"));
        }
        return;
    }

    // 'X' or 'N' (Don't care / Untested)
    const QColor col = pickColor(th.stateX);
    QRect band(x0, yHigh, w, yLow - yHigh);
    QColor fill = col;
    fill.setAlpha(65);
    p.fillRect(band, QBrush(fill, Qt::DiagCrossPattern));
    p.setPen(QPen(col, 1.3));
    p.drawLine(x0, yHigh, x1, yHigh);
    p.drawLine(x0, yLow, x1, yLow);
    if (w >= 20 && st == QLatin1Char('N')) {
        QFont tf = p.font();
        tf.setPointSizeF(7.5);
        p.setFont(tf);
        p.setPen(col.lighter(130));
        p.drawText(band, Qt::AlignCenter, QStringLiteral("N"));
    }
}

void WaveformEditorWidget::paintWaveforms(QPainter &p, const QRect &rect, int scrollX, int scrollY) const {
    const WaveformColorTheme &th = paintTheme();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect, th.canvasBg);

    if (!doc_ || rows_.isEmpty() || doc_->vectors.isEmpty()) {
        p.setPen(th.stateZ);
        p.drawText(rect, Qt::AlignCenter,
                   QStringLiteral("Open a .SI / .WO file or add signals & vectors to begin."));
        return;
    }

    const int nVecs = doc_->vectors.size();
    const int nRows = rows_.size();

    const int vStart = qMax(0, scrollX / cellWidth_ - 1);
    const int vEnd = qMin(nVecs - 1, (scrollX + rect.width()) / cellWidth_ + 1);
    const int rStart = qMax(0, scrollY / rowHeight_);
    const int rEnd = qMin(nRows - 1, (scrollY + rect.height()) / rowHeight_ + 1);

    int selR0, selR1, selV0, selV1;
    selectedRowRange(selR0, selR1);
    selectedVectorRange(selV0, selV1);

    // Alternating row backgrounds & horizontal separators
    for (int r = rStart; r <= rEnd; ++r) {
        const int y = r * rowHeight_ - scrollY;
        if (r % 2 == 1) {
            p.fillRect(QRect(0, y, rect.width(), rowHeight_), th.canvasAltRowBg);
        }
        if (showGrid_) {
            p.setPen(th.gridLine);
            p.drawLine(0, y + rowHeight_ - 1, rect.width(), y + rowHeight_ - 1);
        }
    }

    // Vertical grid lines
    if (showGrid_) {
        for (int v = vStart; v <= vEnd + 1; ++v) {
            const int x = v * cellWidth_ - scrollX;
            const bool major = (v % 5 == 0);
            p.setPen(major ? th.gridMajorLine : th.gridLine);
            p.drawLine(x, 0, x, qMin(rect.height(), nRows * rowHeight_ - scrollY));
        }
    }

    // Without colours a simulation mismatch is a dotted cell with a heavy border.
    const QBrush errorFill = monochrome_ ? QBrush(th.errorBorder, Qt::Dense7Pattern) : QBrush(th.errorBg);
    const qreal errorPenScale = monochrome_ ? 1.6 : 1.0;

    // Selection rectangle highlight (not printed in black and white)
    if (!monochrome_) {
        const int sx0 = selV0 * cellWidth_ - scrollX;
        const int sx1 = (selV1 + 1) * cellWidth_ - scrollX;
        const int sy0 = selR0 * rowHeight_ - scrollY;
        const int sy1 = (selR1 + 1) * rowHeight_ - scrollY;
        const QRect selRect(QPoint(sx0, sy0), QPoint(sx1 - 1, sy1 - 1));
        if (selRect.intersects(rect)) {
            p.fillRect(selRect.intersected(rect), th.selectionFill);
            p.setPen(QPen(th.state0.lighter(115), 1.0, Qt::DashLine));
            p.drawRect(selRect);
        }
    }

    QFont busFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    busFont.setPointSizeF(9.5);
    busFont.setBold(true);

    // Draw waveforms row by row
    for (int r = rStart; r <= rEnd; ++r) {
        const int y = r * rowHeight_ - scrollY;
        const VisibleWaveformRow &vr = rows_[r];
        const SignalItem &sig = doc_->signalsList[vr.sigIndex];

        if (!vr.isBusMember() && sig.isBus()) {
            // Highlight any #E error cells on bus row first
            for (int v = vStart; v <= vEnd; ++v) {
                if (doc_->hasErrorInSignal(v, sig)) {
                    QRect errRect(v * cellWidth_ - scrollX, y + 1, cellWidth_, rowHeight_ - 2);
                    p.fillRect(errRect, errorFill);
                    p.setPen(QPen(th.errorBorder, 1.4 * errorPenScale));
                    p.drawRect(errRect.adjusted(0, 0, -1, -1));
                }
            }

            // Merge consecutive vectors with identical bus bit pattern into clean hexagonal spans!
            int spanStart = vStart;
            while (spanStart > 0 &&
                   doc_->busBitString(sig, spanStart - 1) == doc_->busBitString(sig, spanStart)) {
                spanStart--;
            }

            const int padY = qMax(5, rowHeight_ / 5);
            const int yHigh = y + padY;
            const int yLow = y + rowHeight_ - 1 - padY;
            const int yMid = (yHigh + yLow) / 2;
            const QColor strokeCol = signalColor(sig).isValid() ? signalColor(sig) : th.busStroke;

            int v = spanStart;
            while (v <= vEnd) {
                const QString pattern = doc_->busBitString(sig, v);
                int spanEnd = v;
                while (spanEnd + 1 < nVecs && doc_->busBitString(sig, spanEnd + 1) == pattern) {
                    spanEnd++;
                }

                const int xLeft = v * cellWidth_ - scrollX;
                const int xRight = (spanEnd + 1) * cellWidth_ - scrollX;
                const int spanW = xRight - xLeft;

                bool allZ = true, allX = true, allSim = true;
                for (QChar bc : pattern) {
                    if (bc != QLatin1Char('Z')) allZ = false;
                    if (bc != QLatin1Char('X') && bc != QLatin1Char('N')) allX = false;
                    if (bc != QLatin1Char('*')) allSim = false;
                }

                if (allZ) {
                    p.setPen(QPen(th.stateZ, 2.0, Qt::DashLine));
                    p.drawLine(xLeft, yMid, xRight, yMid);
                    if (spanW >= 42) {
                        p.setFont(busFont);
                        p.setPen(th.stateZ);
                        p.drawText(QRect(xLeft + 4, yHigh - 2, spanW - 8, yLow - yHigh + 4),
                                   Qt::AlignCenter, QStringLiteral("High-Z"));
                    }
                } else {
                    const qreal slant = qMin<qreal>(5.0, qMax<qreal>(2.0, spanW * 0.15));
                    const qreal leftSlant = (v > 0) ? slant : 0.0;
                    const qreal rightSlant = (spanEnd < nVecs - 1) ? slant : 0.0;

                    QPainterPath hexPath;
                    hexPath.moveTo(xLeft, yMid);
                    hexPath.lineTo(xLeft + leftSlant, yHigh);
                    hexPath.lineTo(xRight - rightSlant, yHigh);
                    hexPath.lineTo(xRight, yMid);
                    hexPath.lineTo(xRight - rightSlant, yLow);
                    hexPath.lineTo(xLeft + leftSlant, yLow);
                    hexPath.closeSubpath();

                    if (allX) {
                        QColor fCol = th.stateX;
                        fCol.setAlpha(65);
                        p.fillPath(hexPath, QBrush(fCol, Qt::DiagCrossPattern));
                        p.setPen(QPen(th.stateX, 1.6));
                        p.drawPath(hexPath);
                    } else if (allSim) {
                        QColor fCol = th.stateSim;
                        fCol.setAlpha(55);
                        p.fillPath(hexPath, QBrush(fCol, Qt::FDiagPattern));
                        p.setPen(QPen(th.stateSim, 1.6, Qt::DotLine));
                        p.drawPath(hexPath);
                    } else {
                        QColor fillCol = signalColor(sig).isValid()
                                             ? QColor(signalColor(sig).red(), signalColor(sig).green(), signalColor(sig).blue(), 48)
                                             : th.busFill;
                        p.fillPath(hexPath, fillCol);
                        p.setPen(QPen(strokeCol, 1.8));
                        p.drawPath(hexPath);
                    }

                    // Value text inside bus envelope (clamped to visible viewport so wide spans stay readable!)
                    const int visL = qMax(xLeft + static_cast<int>(leftSlant) + 3, 2);
                    const int visR = qMin(xRight - static_cast<int>(rightSlant) - 3, rect.width() - 2);
                    const int availW = visR - visL;
                    if (availW >= 14) {
                        p.setFont(busFont);
                        p.setPen(allSim ? th.stateSim : (allX ? th.stateX.lighter(135) : th.busText));
                        const QString valStr = doc_->formatBusValue(sig, v, sig.displayRadix);
                        QFontMetrics fm(busFont);
                        const QString elided = fm.elidedText(valStr, Qt::ElideRight, availW);
                        p.drawText(QRect(visL, yHigh, availW, yLow - yHigh), Qt::AlignCenter, elided);
                    }
                }

                v = spanEnd + 1;
            }
        } else {
            // Single-bit row (Scalar or expanded Bus member)
            const QString pin = vr.isBusMember() ? sig.members[vr.memberIndex] : sig.members.first();
            for (int v = vStart; v <= vEnd; ++v) {
                const QRect cellRect(v * cellWidth_ - scrollX, y, cellWidth_, rowHeight_ - 1);
                if (const SimError *se = doc_->findError(v, pin)) {
                    p.fillRect(cellRect.adjusted(0, 1, 0, 0), errorFill);
                    p.setPen(QPen(th.errorBorder, 1.3 * errorPenScale));
                    p.drawRect(cellRect.adjusted(0, 1, -1, -1));
                    if (cellWidth_ >= 34) {
                        QFont ef = font();
                        ef.setPointSizeF(7.0);
                        p.setFont(ef);
                        p.setPen(th.errorBorder.lighter(120));
                        p.drawText(cellRect.adjusted(2, 1, -2, -1), Qt::AlignTop | Qt::AlignRight,
                                   QStringLiteral("exp:%1").arg(se->expected));
                    }
                }
                const QChar prevSt = (v > 0) ? doc_->pinState(v - 1, pin) : QLatin1Char('\0');
                const QChar curSt = doc_->pinState(v, pin);
                drawSingleBitCell(p, cellRect, prevSt, curSt, signalColor(sig));
            }
        }
    }

    if (monochrome_) return;

    // Secondary reference marker line
    if (markerVec_ >= vStart && markerVec_ <= vEnd) {
        const int mx = markerVec_ * cellWidth_ + cellWidth_ / 2 - scrollX;
        p.setPen(QPen(th.markerLine, 1.3, Qt::DashDotLine));
        p.drawLine(mx, 0, mx, rect.height());
    }

    // Primary cursor line
    if (cursorVec_ >= vStart && cursorVec_ <= vEnd) {
        const int cx = cursorVec_ * cellWidth_ + cellWidth_ / 2 - scrollX;
        QColor cLine = th.cursorLine;
        cLine.setAlpha(190);
        p.setPen(QPen(cLine, 1.4));
        p.drawLine(cx, 0, cx, rect.height());
    }
}

void WaveformEditorWidget::showCanvasContextMenu(const QPoint &globalPos, int rowIdx, int vecIdx) {
    if (!doc_ || rowIdx < 0 || rowIdx >= rows_.size()) return;
    const VisibleWaveformRow &vr = rows_[rowIdx];
    SignalItem &sig = doc_->signalsList[vr.sigIndex];

    QMenu menu(this);

    if (!vr.isBusMember() && sig.isBus()) {
        int v0, v1;
        selectedVectorRange(v0, v1);
        QAction *actSetBus = menu.addAction(QStringLiteral("Set Bus Value...\tEnter"));
        connect(actSetBus, &QAction::triggered, this, [this, sigIdx = vr.sigIndex, v0, v1]() {
            emit requestSetBusValue(sigIdx, v0, v1);
        });

        QMenu *radixMenu = menu.addMenu(QStringLiteral("Bus Display Radix"));
        auto addRadixAct = [&](const QString &label, BusRadix r) {
            QAction *a = radixMenu->addAction(label);
            a->setCheckable(true);
            a->setChecked(sig.displayRadix == r);
            connect(a, &QAction::triggered, this, [this, sigIdx = vr.sigIndex, r]() {
                doc_->signalsList[sigIdx].displayRadix = r;
                refreshAll();
                emitCursorStatus();
            });
        };
        addRadixAct(QStringLiteral("Hexadecimal"), BusRadix::Hex);
        addRadixAct(QStringLiteral("Binary"), BusRadix::Binary);
        addRadixAct(QStringLiteral("Octal"), BusRadix::Octal);
        addRadixAct(QStringLiteral("Decimal"), BusRadix::Decimal);

        QAction *actExpand = menu.addAction(sig.expanded ? QStringLiteral("Collapse Bus Members")
                                                         : QStringLiteral("Expand Bus Members"));
        connect(actExpand, &QAction::triggered, this, [this, sigIdx = vr.sigIndex]() {
            toggleBusExpanded(sigIdx);
        });
        menu.addSeparator();
    }

    struct StateEntry {
        QChar ch;
        const char *label;
    };
    static const StateEntry entries[] = {
        {QLatin1Char('0'), "0 - Drive Input Low"},
        {QLatin1Char('1'), "1 - Drive Input High"},
        {QLatin1Char('L'), "L - Test Output Low"},
        {QLatin1Char('H'), "H - Test Output High"},
        {QLatin1Char('Z'), "Z - Test High Impedance"},
        {QLatin1Char('X'), "X - Don't Care / Untested"},
        {QLatin1Char('*'), "* - Simulator Determined"},
        {QLatin1Char('P'), "P - Register Preload"},
        {QLatin1Char('C'), "C - Clock Pulse (0-1-0)"},
        {QLatin1Char('K'), "K - Clock Pulse (1-0-1)"},
    };

    for (const auto &e : entries) {
        QAction *a = menu.addAction(QString::fromLatin1(e.label));
        connect(a, &QAction::triggered, this, [this, ch = e.ch]() { applyStateToSelection(ch); });
    }

    menu.addSeparator();
    QMenu *wholeMenu = menu.addMenu(QStringLiteral("Set Whole Signal (%1)").arg(sig.displayLabel()));
    for (const auto &e : entries) {
        QAction *a = wholeMenu->addAction(QString::fromLatin1(e.label));
        connect(a, &QAction::triggered, this, [this, sigIdx = vr.sigIndex, ch = e.ch]() {
            applyStateToWholeSignal(sigIdx, ch);
        });
    }

    menu.addSeparator();
    QAction *actInsBefore = menu.addAction(QStringLiteral("Insert Vector Before #%1").arg(vecIdx + 1));
    connect(actInsBefore, &QAction::triggered, this, [this, vecIdx]() {
        emit aboutToModifyDocument(QStringLiteral("Insert Vector"));
        doc_->insertVector(vecIdx, 1, QLatin1Char('0'), QLatin1Char('*'), true);
        cursorVec_ = vecIdx;
        selVecAnchor_ = selVecEnd_ = vecIdx;
        refreshAll();
        emit documentModified();
        emitCursorStatus();
    });

    QAction *actDup = menu.addAction(QStringLiteral("Duplicate Vector #%1").arg(vecIdx + 1));
    connect(actDup, &QAction::triggered, this, [this, vecIdx]() {
        emit aboutToModifyDocument(QStringLiteral("Duplicate Vector"));
        doc_->duplicateVector(vecIdx);
        cursorVec_ = vecIdx + 1;
        selVecAnchor_ = selVecEnd_ = cursorVec_;
        refreshAll();
        emit documentModified();
        emitCursorStatus();
    });

    QAction *actComment = menu.addAction(QStringLiteral("Edit Vector Comment / $MSG..."));
    connect(actComment, &QAction::triggered, this, [this, vecIdx]() {
        emit requestEditVectorAnnotation(vecIdx);
    });

    int v0, v1;
    selectedVectorRange(v0, v1);
    QAction *actDelVec = menu.addAction(v1 > v0
                                            ? QStringLiteral("Delete Vectors #%1..#%2").arg(v0 + 1).arg(v1 + 1)
                                            : QStringLiteral("Delete Vector #%1").arg(v0 + 1));
    connect(actDelVec, &QAction::triggered, this, [this, v0, v1]() {
        emit aboutToModifyDocument(QStringLiteral("Delete Vectors"));
        doc_->deleteVectors(v0, v1);
        if (!doc_->vectors.isEmpty()) {
            cursorVec_ = qBound(0, v0, doc_->vectors.size() - 1);
            selVecAnchor_ = selVecEnd_ = cursorVec_;
        }
        refreshAll();
        emit documentModified();
        emitCursorStatus();
    });

    menu.exec(globalPos);
}

void WaveformEditorWidget::showHeaderContextMenu(const QPoint &globalPos, int rowIdx) {
    if (!doc_ || rowIdx < 0 || rowIdx >= rows_.size()) return;
    const VisibleWaveformRow &vr = rows_[rowIdx];
    const int sigIdx = vr.sigIndex;
    SignalItem &sig = doc_->signalsList[sigIdx];

    QMenu menu(this);
    QAction *actAdd = menu.addAction(QStringLiteral("Add Signal / Bus..."));
    connect(actAdd, &QAction::triggered, this, &WaveformEditorWidget::requestAddSignal);

    QAction *actRen = menu.addAction(QStringLiteral("Rename Signal '%1'...").arg(sig.displayLabel()));
    connect(actRen, &QAction::triggered, this, [this, sigIdx]() { emit requestRenameSignal(sigIdx); });

    QAction *actDel = menu.addAction(QStringLiteral("Delete Signal '%1'").arg(sig.displayLabel()));
    connect(actDel, &QAction::triggered, this, [this, sigIdx]() { emit requestDeleteSignal(sigIdx); });

    menu.addSeparator();
    QAction *actUp = menu.addAction(QStringLiteral("Move Signal Up"));
    actUp->setEnabled(sigIdx > 0);
    connect(actUp, &QAction::triggered, this, [this, sigIdx]() { emit requestMoveSignal(sigIdx, sigIdx - 1); });

    QAction *actDown = menu.addAction(QStringLiteral("Move Signal Down"));
    actDown->setEnabled(sigIdx + 1 < doc_->signalsList.size());
    connect(actDown, &QAction::triggered, this, [this, sigIdx]() { emit requestMoveSignal(sigIdx, sigIdx + 1); });

    if (sig.isBus()) {
        menu.addSeparator();
        QAction *actToggle = menu.addAction(sig.expanded ? QStringLiteral("Collapse Bus Members")
                                                         : QStringLiteral("Expand Bus Members"));
        connect(actToggle, &QAction::triggered, this, [this, sigIdx]() { toggleBusExpanded(sigIdx); });

        QMenu *radixMenu = menu.addMenu(QStringLiteral("Bus Display Radix"));
        auto addRadixAct = [&](const QString &label, BusRadix r) {
            QAction *a = radixMenu->addAction(label);
            a->setCheckable(true);
            a->setChecked(sig.displayRadix == r);
            connect(a, &QAction::triggered, this, [this, sigIdx, r]() {
                doc_->signalsList[sigIdx].displayRadix = r;
                refreshAll();
                emitCursorStatus();
            });
        };
        addRadixAct(QStringLiteral("Hexadecimal"), BusRadix::Hex);
        addRadixAct(QStringLiteral("Binary"), BusRadix::Binary);
        addRadixAct(QStringLiteral("Octal"), BusRadix::Octal);
        addRadixAct(QStringLiteral("Decimal"), BusRadix::Decimal);
    }

    menu.addSeparator();
    QAction *actColor = menu.addAction(QStringLiteral("Signal Color..."));
    connect(actColor, &QAction::triggered, this, [this, sigIdx]() {
        const QColor initial = doc_->signalsList[sigIdx].customColor.isValid()
                                   ? doc_->signalsList[sigIdx].customColor
                                   : theme_.state0;
        QColor chosen = QColorDialog::getColor(initial, this, QStringLiteral("Choose Signal Color"));
        if (chosen.isValid()) {
            doc_->signalsList[sigIdx].customColor = chosen;
            refreshAll();
        }
    });

    if (sig.customColor.isValid()) {
        QAction *actResetCol = menu.addAction(QStringLiteral("Reset Signal Color"));
        connect(actResetCol, &QAction::triggered, this, [this, sigIdx]() {
            doc_->signalsList[sigIdx].customColor = QColor();
            refreshAll();
        });
    }

    menu.exec(globalPos);
}

void WaveformEditorWidget::showRulerContextMenu(const QPoint &globalPos, int vecIdx) {
    if (!doc_) return;
    QMenu menu(this);

    QAction *actAddDlg = menu.addAction(QStringLiteral("Add Vectors..."));
    connect(actAddDlg, &QAction::triggered, this, &WaveformEditorWidget::requestAddVectorDialog);

    QAction *actInsBefore = menu.addAction(QStringLiteral("Insert Vector Before #%1").arg(vecIdx + 1));
    connect(actInsBefore, &QAction::triggered, this, [this, vecIdx]() {
        emit aboutToModifyDocument(QStringLiteral("Insert Vector"));
        doc_->insertVector(vecIdx, 1, QLatin1Char('0'), QLatin1Char('*'), true);
        cursorVec_ = vecIdx;
        selVecAnchor_ = selVecEnd_ = vecIdx;
        refreshAll();
        emit documentModified();
        emitCursorStatus();
    });

    QAction *actDup = menu.addAction(QStringLiteral("Duplicate Vector #%1").arg(vecIdx + 1));
    connect(actDup, &QAction::triggered, this, [this, vecIdx]() {
        emit aboutToModifyDocument(QStringLiteral("Duplicate Vector"));
        doc_->duplicateVector(vecIdx);
        cursorVec_ = vecIdx + 1;
        selVecAnchor_ = selVecEnd_ = cursorVec_;
        refreshAll();
        emit documentModified();
        emitCursorStatus();
    });

    QAction *actMsg = menu.addAction(QStringLiteral("Edit Vector Comment / $MSG..."));
    connect(actMsg, &QAction::triggered, this, [this, vecIdx]() {
        emit requestEditVectorAnnotation(vecIdx);
    });

    QAction *actMarker = menu.addAction(QStringLiteral("Set Measurement Reference Marker Here"));
    connect(actMarker, &QAction::triggered, this, [this, vecIdx]() {
        markerVec_ = vecIdx;
        refreshAll();
        emitCursorStatus();
    });

    int v0, v1;
    selectedVectorRange(v0, v1);
    menu.addSeparator();
    QAction *actDel = menu.addAction(v1 > v0
                                         ? QStringLiteral("Delete Vectors #%1..#%2").arg(v0 + 1).arg(v1 + 1)
                                         : QStringLiteral("Delete Vector #%1").arg(v0 + 1));
    connect(actDel, &QAction::triggered, this, [this, v0, v1]() {
        emit aboutToModifyDocument(QStringLiteral("Delete Vectors"));
        doc_->deleteVectors(v0, v1);
        if (!doc_->vectors.isEmpty()) {
            cursorVec_ = qBound(0, v0, doc_->vectors.size() - 1);
            selVecAnchor_ = selVecEnd_ = cursorVec_;
        }
        refreshAll();
        emit documentModified();
        emitCursorStatus();
    });

    menu.exec(globalPos);
}

const WaveformColorTheme &WaveformEditorWidget::paintTheme() const {
    static const WaveformColorTheme mono = WaveformColorTheme::monochrome();
    return monochrome_ ? mono : theme_;
}

QColor WaveformEditorWidget::signalColor(const SignalItem &sig) const {
    return monochrome_ ? QColor() : sig.customColor;
}

QImage WaveformEditorWidget::renderToImage(int maxVectors, bool monochrome) const {
    // The paint functions read monochrome_ through paintTheme() and
    // signalColor(); it is only set while this image is painted.
    monochrome_ = monochrome;
    const WaveformColorTheme &th = paintTheme();

    const int nVecs = doc_ ? doc_->vectors.size() : 0;
    const int drawVecs = (maxVectors > 0 && nVecs > maxVectors) ? maxVectors : qMax(8, nVecs);
    const int drawRows = qMax(3, rows_.size());

    const int waveW = qBound(320, drawVecs * cellWidth_, 16384);
    const int waveH = qBound(120, drawRows * rowHeight_, 16384);
    const int imgW = headerWidth_ + waveW;
    const int imgH = rulerHeight_ + waveH;

    QImage img(imgW, imgH, QImage::Format_ARGB32_Premultiplied);
    img.fill(th.canvasBg);

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);

    p.save();
    p.setClipRect(0, 0, headerWidth_, rulerHeight_);
    paintCorner(p, QRect(0, 0, headerWidth_, rulerHeight_));
    p.restore();

    p.save();
    p.translate(headerWidth_, 0);
    p.setClipRect(0, 0, waveW, rulerHeight_);
    paintRuler(p, QRect(0, 0, waveW, rulerHeight_), 0);
    p.restore();

    p.save();
    p.translate(0, rulerHeight_);
    p.setClipRect(0, 0, headerWidth_, waveH);
    paintHeaders(p, QRect(0, 0, headerWidth_, waveH), 0);
    p.restore();

    p.save();
    p.translate(headerWidth_, rulerHeight_);
    p.setClipRect(0, 0, waveW, waveH);
    paintWaveforms(p, QRect(0, 0, waveW, waveH), 0, 0);
    p.restore();

    monochrome_ = false;
    return img;
}

bool WaveformEditorWidget::exportPng(const QString &path, QString *errorOut) const {
    const QImage img = renderToImage(512);
    if (!img.save(path, "PNG")) {
        if (errorOut) *errorOut = QStringLiteral("Failed to write PNG image to %1").arg(path);
        return false;
    }
    return true;
}

bool WaveformEditorWidget::exportPdf(const QString &path, QString *errorOut, bool colors) const {
#ifdef LINSIM_HAS_PRINTSUPPORT
    QPdfWriter pdf(path);
    pdf.setPageSize(QPageSize(QPageSize::A4));
    pdf.setPageOrientation(QPageLayout::Landscape);
    pdf.setResolution(300);
    pdf.setTitle(doc_ && !doc_->header.name.isEmpty()
                     ? QStringLiteral("LinSim Waveform - %1").arg(doc_->header.name)
                     : QStringLiteral("LinSim Waveform"));

    QPainter painter(&pdf);
    if (!painter.isActive()) {
        if (errorOut) *errorOut = QStringLiteral("Could not open PDF writer for %1").arg(path);
        return false;
    }

    const QRect pageRect = painter.viewport();
    QFont titleFont = font();
    titleFont.setBold(true);
    titleFont.setPointSize(11);
    painter.setFont(titleFont);
    painter.setPen(Qt::black);

    const QString title = doc_
                              ? QStringLiteral("Design: %1   |   Device: %2   |   Vectors: %3")
                                    .arg(doc_->header.name, doc_->header.device)
                                    .arg(doc_->vectors.size())
                              : QStringLiteral("LinSim Waveform");
    const int titleH = painter.fontMetrics().height() + 24;
    painter.drawText(QRect(0, 0, pageRect.width(), titleH), Qt::AlignLeft | Qt::AlignVCenter, title);

    const QImage img = renderToImage(256, !colors);
    const QRect targetArea(0, titleH, pageRect.width(), pageRect.height() - titleH);
    const QSize scaled = img.size().scaled(targetArea.size(), Qt::KeepAspectRatio);
    painter.drawImage(QRect(targetArea.topLeft(), scaled), img);
    painter.end();
    return true;
#else
    Q_UNUSED(path);
    Q_UNUSED(colors);
    if (errorOut) *errorOut = QStringLiteral("Qt6::PrintSupport was not available at build time.");
    return false;
#endif
}
