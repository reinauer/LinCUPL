#pragma once

#include <QDialog>
#include <QList>
#include <QString>
#include <QStringList>

class QButtonGroup;
class QCheckBox;
class QLabel;
class QLineEdit;
class QSettings;

// Options of Options > Compiler Options, each one a cupl flag.  The
// labels say what cupl does with the flag (measured with cupl itself), not
// what cupl's usage text claims.
struct CompilerOptions {
    // General
    bool secureDevice = false;       // -g: program the security fuse (G1 in the .jed)
    bool deactivateUnusedOr = false; // -d: IFL devices, unused OR terms switched off
    bool simulate = false;           // -s: run the simulator on the .si after compiling
    bool oneHot = false;             // -o: SEQUENCE states are one-hot codes
    bool jedecNameIsPldName = false; // -n: .jed (and .hex) named after the design file

    // Minimization
    int minimization = 1;            // -m0 .. -m4
    bool bestPolarity = false;       // -kb
    bool demorgan = false;           // -kd
    bool keepXor = false;            // -kx
    bool shareTerms = false;         // -ks
    bool noSharing = false;          // -r

    // Download
    bool jedec = true;               // -j: .jed (runs the fitter for fitter devices)
    bool hlFile = false;             // -i: .hl for Signetics IFL devices
    bool asciiHex = false;           // -h: .hex for PROMs

    // Output
    bool edif = false;               // -t: .edf
    bool palasm = false;             // -c: .pds
    bool pla = false;                // -b: .pla
    bool pdif = false;               // -p: .pdf
    bool expandedMacro = false;      // -e: .mx
    bool list = true;                // -l: .lst
    bool absolute = true;            // -a: .abs and .sim

    // Doc file
    bool equations = true;           // -x: .doc with the equations and symbol table
    bool fusePlot = false;           // -f: fuse plot in the .doc

    // One check box of the dialog: the option it sets, the flag that does it
    // and where it is shown.
    struct Box {
        const char *key;          // QSettings key and object name of the check box
        const char *group;        // group box of the dialog
        const char *label;
        const char *flag;
        const char *tip;
        bool CompilerOptions::*member;
        bool deviceOnly;          // left out when compiling for the virtual device
        const char *exclusiveWith;// key of a box that cannot be on at the same time
    };
    static const QList<Box> &boxes();

    void loadFromSettings();
    void saveToSettings() const;
    // Converts the options of earlier LinCUPL versions (group "compiler"),
    // keeping only those whose check box asked for what its flag does, and
    // removes them.  Called by loadFromSettings.
    static void migrateLegacySettings(QSettings &s);
    QStringList buildFlagArgs(bool deviceIndependent = false) const;
};

// Options of Options > Simulator Options, the csim flags.
struct SimulatorOptions {
    bool writeListing = true;         // -l: .so listing and .wo waveform file
    bool appendVectorsToJedec = false;// -j: test vectors added to the .jed
    bool usePldFilename = false;      // -n: the .jed is named after the design file
    bool autoOpenLinSim = false;      // open LinSim after a successful simulation

    void loadFromSettings();
    void saveToSettings() const;
    static void migrateLegacySettings(QSettings &s);
    QStringList buildFlagArgs(bool deviceIndependent = false) const;
};

class CompilerOptionsDialog : public QDialog {
    Q_OBJECT

public:
    explicit CompilerOptionsDialog(const CompilerOptions &initial, QWidget *parent = nullptr);
    CompilerOptions options() const;

private slots:
    void updatePreview();
    void restoreDefaults();

private:
    void applyToWidgets(const CompilerOptions &opts);

    QButtonGroup *m_minGroup = nullptr;
    QList<QCheckBox *> m_boxes;      // in the order of CompilerOptions::boxes()
    QLineEdit *m_previewEdit = nullptr;
};

class SimulatorOptionsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SimulatorOptionsDialog(const SimulatorOptions &initial, QWidget *parent = nullptr);
    SimulatorOptions options() const;

private slots:
    void updatePreview();
    void restoreDefaults();

private:
    void applyToWidgets(const SimulatorOptions &opts);

    QCheckBox *m_chkListing = nullptr;
    QCheckBox *m_chkAppendJed = nullptr;
    QCheckBox *m_chkUsePldName = nullptr;
    QCheckBox *m_chkAutoLinSim = nullptr;
    QLineEdit *m_previewEdit = nullptr;
};

class EnvironmentPathsDialog : public QDialog {
    Q_OBJECT

public:
    explicit EnvironmentPathsDialog(QWidget *parent = nullptr);

    QString selectedLibraryPath() const;
    QString customBinDir() const;

private slots:
    void browseLibrary();
    void browseBinDir();
    void refreshDetectedTools();

private:
    QLineEdit *m_libEdit = nullptr;
    QLineEdit *m_binDirEdit = nullptr;
    QLabel *m_statusLabel = nullptr;
};
