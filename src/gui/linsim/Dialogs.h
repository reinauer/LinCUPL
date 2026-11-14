#pragma once

#include "ColorTheme.h"
#include "SimDocument.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QSpinBox;

class DesignPropertiesDialog : public QDialog {
    Q_OBJECT

public:
    explicit DesignPropertiesDialog(const SimHeader &hdr, BusRadix base, QWidget *parent = nullptr);

    SimHeader headerResult() const;
    BusRadix baseResult() const;

private:
    QLineEdit *nameEdit_ = nullptr;
    QLineEdit *partNoEdit_ = nullptr;
    QLineEdit *dateEdit_ = nullptr;
    QLineEdit *revEdit_ = nullptr;
    QLineEdit *designerEdit_ = nullptr;
    QLineEdit *companyEdit_ = nullptr;
    QLineEdit *assemblyEdit_ = nullptr;
    QLineEdit *locationEdit_ = nullptr;
    QLineEdit *deviceEdit_ = nullptr;
    QComboBox *baseCombo_ = nullptr;
};

class AddSignalDialog : public QDialog {
    Q_OBJECT

public:
    explicit AddSignalDialog(const SimDocument &doc, QWidget *parent = nullptr);

    bool buildSignalItem(SignalItem &outItem, bool &insertBeforeSelected, QString *errorOut) const;

private:
    const SimDocument &doc_;
    QListWidget *availableList_ = nullptr;
    QLineEdit *nameEdit_ = nullptr;
    QCheckBox *activeLowCheck_ = nullptr;
    QCheckBox *isBusCheck_ = nullptr;
    QLineEdit *membersEdit_ = nullptr;
    QComboBox *posCombo_ = nullptr;
};

class AddVectorDialog : public QDialog {
    Q_OBJECT

public:
    enum class Position { AppendEnd, InsertBeforeCursor, InsertAfterCursor };
    enum class InitMode { CopyNeighbor, DefaultIn0OutSim, AllDontCare };

    explicit AddVectorDialog(int currentVec, int totalVecs, QWidget *parent = nullptr);

    int countResult() const;
    Position positionResult() const;
    InitMode initModeResult() const;

private:
    QSpinBox *countSpin_ = nullptr;
    QComboBox *posCombo_ = nullptr;
    QComboBox *initCombo_ = nullptr;
};

class SetBusValueDialog : public QDialog {
    Q_OBJECT

public:
    explicit SetBusValueDialog(const SimDocument &doc, int sigIdx, int startVec, int endVec,
                               QWidget *parent = nullptr);

    QString valueText() const;
    BusRadix radix() const;
    bool driveAsOutput() const;
    int startVector() const;
    int endVector() const;

private:
    QLineEdit *valEdit_ = nullptr;
    QComboBox *radixCombo_ = nullptr;
    QComboBox *driveCombo_ = nullptr;
    QSpinBox *startSpin_ = nullptr;
    QSpinBox *endSpin_ = nullptr;
};

class GridSizeDialog : public QDialog {
    Q_OBJECT

public:
    GridSizeDialog(int cellW, int rowH, bool showGrid, bool wideSignals, QWidget *parent = nullptr);

    int cellWidth() const;
    int rowHeight() const;
    bool showGrid() const;
    bool wideSignals() const;

private:
    QSpinBox *cellWSpin_ = nullptr;
    QSpinBox *rowHSpin_ = nullptr;
    QCheckBox *gridCheck_ = nullptr;
    QCheckBox *wideCheck_ = nullptr;
};

class WaveformColorsDialog : public QDialog {
    Q_OBJECT

public:
    explicit WaveformColorsDialog(const WaveformColorTheme &initial, QWidget *parent = nullptr);

    WaveformColorTheme themeResult() const { return theme_; }

private:
    void refreshButtons();

    WaveformColorTheme theme_;
    struct Swatch {
        QColor WaveformColorTheme::*member;
        class QPushButton *btn;
    };
    QVector<Swatch> swatches_;
};

class VectorAnnotationDialog : public QDialog {
    Q_OBJECT

public:
    VectorAnnotationDialog(int vecIndex, const QString &comment, const QStringList &messages,
                           QWidget *parent = nullptr);

    QString commentResult() const;
    QStringList messagesResult() const;

private:
    QLineEdit *commentEdit_ = nullptr;
    QPlainTextEdit *msgEdit_ = nullptr;
};

class SetLibraryDialog : public QDialog {
    Q_OBJECT

public:
    SetLibraryDialog(const QString &currentLib, const QStringList &discoveredLibs,
                     bool appendJedec, bool usePldName, QWidget *parent = nullptr);

    QString libraryPath() const;
    bool appendJedecFlag() const;
    bool usePldNameFlag() const;

private:
    QComboBox *libCombo_ = nullptr;
    QCheckBox *jedecCheck_ = nullptr;
    QCheckBox *pldNameCheck_ = nullptr;
};

// Non-modal Find box of a LinSim window (Edit -> Find...).  The window does
// the searching: in the waveform editor it looks for a signal name, a vector
// number or a vector comment / $MSG text; in the .SO and .SI tabs it looks
// for text in the editor.
class FindDialog : public QDialog {
    Q_OBJECT

public:
    enum class Target { SignalName, VectorNumber, VectorText };

    explicit FindDialog(QWidget *parent = nullptr);

    void focusFind(const QString &seedText = QString());
    // Names what Find Next will search; the "Look for" choice only applies to
    // the waveform editor.
    void setScope(const QString &scopeName, bool waveform);
    void setStatus(const QString &message);

    QString text() const;
    void setText(const QString &text);
    Target target() const;
    void setTarget(Target target);
    bool caseSensitive() const;
    bool wholeWords() const;

signals:
    void findRequested(bool backward);

private:
    QLineEdit *findEdit_ = nullptr;
    QComboBox *targetCombo_ = nullptr;
    QLabel *scopeLabel_ = nullptr;
    QCheckBox *caseCheck_ = nullptr;
    QCheckBox *wholeCheck_ = nullptr;
    QLabel *statusLabel_ = nullptr;
};
