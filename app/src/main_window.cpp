// SPDX-License-Identifier: GPL-3.0-or-later
#include "main_window.h"

#include "ctgp_convert.h"
#include "setup_page.h"
#include "updater.h"
#include "portable.h"

#include <QThread>
#include <QTimer>

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QCoreApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QInputDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QThread>
#include <QPushButton>
#include <QFrame>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <iterator>

namespace {

// Values are suyu's enum indices (src/common/settings_enums.h), in enum order.
const QStringList kResolutions{"0.25x", "0.5x", "0.75x", "1x (720p/1080p)", "1.25x", "1.5x",
                               "2x (1440p/2160p)", "3x", "4x", "5x", "6x", "7x", "8x"};
const QStringList kAspects{"16:9", "4:3", "21:9", "16:10", "32:9", "Estirar a la ventana"};
const QStringList kFilters{"Vecino más cercano", "Bilineal", "Bicúbico", "Gaussiano", "Lanczos", "ScaleForce",
                           "AMD FSR", "Área", "Tangente cero", "B-spline", "Mitchell", "Spline-1",
                           "MMPX", "SGSR", "SGSR edge"};
const QStringList kAntiAliasing{"Ninguno", "FXAA", "SMAA"};
const QStringList kVSync{"Desactivada (inmediata)", "Mailbox", "FIFO (vsync)", "FIFO relajada"};
const QStringList kFullscreenModes{"Ventana sin bordes", "Exclusiva"};
const QStringList kConsoleModes{"Baja (portátil, 720p)", "Alta (en base, 1080p)"};

// Emulated console language. Index order is the suyu SDL `Language` enum, one-to-one with
// suyu's `available_language_codes` (the LanguageCode the console reports to the game at boot).
// The default (index 1, English US) matches suyu's own default when no config exists yet.
const QStringList kConsoleLanguages{
    "Japonés",
    "Inglés (EE. UU.)",
    "Francés",
    "Alemán",
    "Italiano",
    "Español (España)",
    "Chino",
    "Coreano",
    "Neerlandés",
    "Portugués",
    "Ruso",
    "Chino (Taiwán, tradicional)",
    "Inglés (Reino Unido)",
    "Francés (Canadá)",
    "Español (Latinoamérica)",
    "Chino (simplificado)",
    "Chino (tradicional)",
    "Portugués (Brasil)",
    "Polaco",
    "Tailandés",
};

constexpr int kFsrFilterIndex = 6; // ScalingFilter::Fsr

const QString kRenderer = QStringLiteral("Renderer");
const QString kCore = QStringLiteral("Core");
const QString kSystem = QStringLiteral("System");
const QString kControls = QStringLiteral("Controls");

// Every tab scrolls instead of clipping: a tiled or small window must never cut a page off.
QWidget* scrolled(QWidget* page) {
    auto* area = new QScrollArea;
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    area->setWidget(page);
    return area;
}

QComboBox* makeCombo(const QStringList& items) {
    auto* combo = new QComboBox;
    combo->addItems(items);
    return combo;
}

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("KartRecomp"));
    resize(820, 600);

    auto* tabs = new QTabWidget;
    setup_page = new SetupPage;
    tabs->addTab(scrolled(setup_page), tr("Instalación"));
    connect(setup_page, &SetupPage::playRequested, this, &MainWindow::launch); // one place starts the game
    tabs->addTab(scrolled(buildPlayTab()), tr("Jugar"));
    tabs->addTab(scrolled(buildDisplayTab()), tr("Pantalla"));
    tabs->addTab(scrolled(buildFrameRateTab()), tr("Velocidad"));
    tabs->addTab(scrolled(buildEnhancementsTab()), tr("Mejoras"));
    tabs->addTab(scrolled(buildControllersTab()), tr("Mandos"));
    tabs->addTab(scrolled(buildMiiTab()), tr("Miis"));
    tabs->addTab(scrolled(buildDlcTab()), tr("DLC"));
    tabs->addTab(scrolled(buildModsTab()), tr("Mods"));
    setCentralWidget(tabs);

    QSettings prefs;
    QString root = prefs.value(QStringLiteral("game_root")).toString();
    if (root.isEmpty()) {
        root = GamePackage::findDefaultRoot();
    }
    // No game folder anywhere (an installed launcher, the AppImage, a fresh Windows folder): the data lives in the XDG folders and the
    // game is the prebuilt engine plus the modules the Setup page builds. A developer checkout keeps its game folder.
#ifdef Q_OS_WIN
    // Windows has no game-folder build: the download is always the engine plus the modules the Setup page builds. A folder
    // remembered from an older launcher (or picked on the Play tab) made Play look for <folder>\mk8d.exe, which never exists,
    // and say "Cannot start" after a successful build. Only a folder that really holds that executable keeps the old mode.
    if (!root.isEmpty() && !QFileInfo(GamePackage(root).executable()).isFile()) root.clear();
#endif
    if (root.isEmpty() || !qEnvironmentVariableIsEmpty("KARTRECOMP_INSTALLED")) {
        const BuilderPaths builder = BuilderPaths::Detect();
        package = GamePackage::Installed(DataLayout::Default(), DataLayout::ConfigRoot(), builder.engine, builder.engineVersion);
        root = package.root();
    }
    QString home = prefs.value(QStringLiteral("suyu_home")).toString();
    if (home.isEmpty()) {
        home = GamePackage::findDefaultSuyuHome(root);
    }
    root_edit->setText(root);
    home_edit->setText(home);
    if (package.isInstalled()) {
        // Nothing to browse for: the folders are fixed, the Setup page picks the user's files.
        root_edit->setReadOnly(true);
        home_edit->setReadOnly(true);
        home_edit->setText(tr("(se elige en la pestaña Instalación)"));
        tabs->setCurrentIndex(IsBuilt(DataLayout::Default(), BuilderPaths::Detect().engineVersion) ? 1 : 0);
        connect(setup_page, &SetupPage::builtChanged, this, [this] { reloadPackage(); });
    }
    reloadPackage();
    connectAutoSave();

    // Updates from the project's GitHub releases: checked at start-up at most once a day, or with the button on the Play tab.
    updater = new Updater(this);
    connect(updater, &Updater::log, this, [this](const QString& line) { log_view->appendPlainText(line); });
    QTimer::singleShot(1500, this, [this] { updater->check(true); });

    connect(&game, &QProcess::readyReadStandardOutput, this, [this] {
        appendLog(QString::fromUtf8(game.readAllStandardOutput()));
    });
    connect(&game, &QProcess::stateChanged, this, [this](QProcess::ProcessState state) {
        const bool running = state != QProcess::NotRunning;
        setup_page->setGameRunning(running);
        play_button->setEnabled(!running);
        stop_button->setEnabled(running);
        // The game rewrites its whole config on exit, so edits made while it runs would
        // be lost. Lock them, then re-read whatever the game saved.
        display_page->setEnabled(!running);
        frame_rate_page->setEnabled(!running);
        enhancements_page->setEnabled(!running);
        refreshMods();
        if (!running) {
            loadSettings();
            loadOverlay();
        }
    });
    connect(&game, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        QString summary = tr("El juego se ha cerrado (código %1).").arg(code);
        if (code == 4 || code == 5) summary += QLatin1Char(' ') + tr("El juego construido no coincide con este motor o no existe: vuelve a construirlo en la pestaña Instalación.");
        QFile report(package.coverageReport());
        if (report.open(QIODevice::ReadOnly | QIODevice::Text)) {
            for (const auto& line : QString::fromUtf8(report.readAll()).split(QLatin1Char('\n'))) {
                if (line.contains(QLatin1String("static -> JIT")) ||
                    line.contains(QLatin1String("blocks per transition"))) {
                    summary += QLatin1Char('\n') + line.trimmed();
                }
            }
        }
        appendLog(summary + QLatin1Char('\n'));
        status_label->setText(summary);
    });
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (setup_page->isBuilding()) {
        const auto answer = QMessageBox::question(this, tr("Construcción en curso"),
            tr("El juego todavía se está construyendo. ¿Parar la construcción y cerrar? (Al construir de nuevo, sigue donde se quedó.)"));
        if (answer != QMessageBox::Yes) {
            event->ignore();
            return;
        }
        setup_page->stopAndWait();
    }
    QMainWindow::closeEvent(event);
}

MainWindow::~MainWindow() {
    if (game.state() != QProcess::NotRunning) {
        game.terminate();
        game.waitForFinished(5000);
    }
}

QWidget* MainWindow::buildPlayTab() {
    auto* page = new QWidget;
    auto* form = new QFormLayout;

    const auto pathRow = [this](QLineEdit*& edit, const QString& title) {
        auto* row = new QHBoxLayout;
        edit = new QLineEdit;
        auto* browse = new QPushButton(tr("Examinar…"));
        row->addWidget(edit);
        row->addWidget(browse);
        connect(browse, &QPushButton::clicked, this, [this, edit, title] {
            const QString dir = QFileDialog::getExistingDirectory(this, title, edit->text());
            Portable::EnterAppDir();
            if (!dir.isEmpty()) {
                edit->setText(dir);
                reloadPackage();
            }
        });
        connect(edit, &QLineEdit::editingFinished, this, [this] { reloadPackage(); });
        return row;
    };
    form->addRow(tr("Carpeta del juego:"), pathRow(root_edit, tr("Elige la carpeta del juego construido")));
    form->addRow(tr("Claves + firmware:"), pathRow(home_edit, tr("Elige la carpeta de datos de suyu con tus claves")));

    language_combo = makeCombo(kConsoleLanguages);
    language_combo->setCurrentIndex(5); // Español (España) until the config says otherwise
    language_combo->setToolTip(
        tr("El idioma en el que la consola emulada ejecuta el juego (textos y voces). Se guarda en la configuración "
           "del juego y se usa en cada arranque hasta que elijas otro aquí (o dentro del juego, donde también se "
           "puede cambiar). Para los idiomas europeos se pone también la región Europa."));
    form->addRow(tr("Idioma de la consola:"), language_combo);

    renderer_combo = makeCombo({tr("GPU emulada (predeterminado)"), tr("Vulkan nativo (experimental)")});
    renderer_combo->setToolTip(
        tr("Vulkan nativo ejecuta las llamadas gráficas NVN del propio juego directamente en tu GPU, sin "
           "GPU emulada. Es nuevo: habrá una breve pausa al empezar cada circuito; por favor, informa "
           "de cualquier cosa que se vea distinta a la GPU emulada. La resolución interna de la pestaña "
           "Pantalla se aplica a los dos renderizadores (mira el escalado de resolución nativa para "
           "desactivarla en nativo); el filtro de escalado, FSR y el antialiasing solo se aplican a la "
           "GPU emulada por ahora."));
    renderer_combo->setCurrentIndex(QSettings().value(QStringLiteral("renderer"), 0).toInt());
    connect(renderer_combo, &QComboBox::currentIndexChanged, this,
            [](int index) { QSettings().setValue(QStringLiteral("renderer"), index); });
    form->addRow(tr("Renderizador:"), renderer_combo);
    native_res_combo = makeCombo({tr("720p"), tr("1080p"), tr("1440p"), tr("4K (2160p)")});
    native_res_combo->setToolTip(
        tr("La resolución de salida del renderizador Vulkan nativo: el tamaño de la imagen final, HUD incluido. "
           "La escena 3D puede dibujarse más pequeña y reescalarse desde el menú del juego (F10). Para Vulkan nativo "
           "sustituye al multiplicador de resolución interna de la pestaña Pantalla; la GPU emulada sigue usando ese."));
    native_res_combo->setCurrentIndex(QSettings().value(QStringLiteral("native_output_resolution"), 2).toInt());
    connect(native_res_combo, &QComboBox::currentIndexChanged, this,
            [](int index) { QSettings().setValue(QStringLiteral("native_output_resolution"), index); });
    native_res_combo->setEnabled(renderer_combo->currentIndex() == 1);
    connect(renderer_combo, &QComboBox::currentIndexChanged, native_res_combo,
            [this](int index) { native_res_combo->setEnabled(index == 1); });
    form->addRow(tr("Resolución de salida nativa:"), native_res_combo);
    auto* ingame_note = new QLabel(
        tr("Dentro del juego, F10 (o pulsar los dos sticks, L3 + R3, en un mando) abre un menú de ajustes: reescalador, escala de "
           "renderizado, nitidez, generación de fotogramas, HDR, modo de presentación, límite de velocidad, contador de FPS y qué "
           "mando usa cada jugador (1 a 4). La pestaña Mejoras tiene los mismos ajustes."));
    ingame_note->setWordWrap(true);
    form->addRow(ingame_note);
#ifdef Q_OS_LINUX
    pin_combo = makeCombo({tr("Desactivado (predeterminado)"), tr("Activado")});
    pin_combo->setToolTip(
        tr("Fija el hilo de cada núcleo de CPU emulado (y el del renderizador nativo) a su propia CPU, para que "
           "las cachés no se vacíen en cada migración. Ayuda un poco en los momentos exigentes (menos "
           "tirones). Déjalo desactivado si la CPU está ocupada con otros programas. Necesita 6 hilos o más."));
    pin_combo->setCurrentIndex(QSettings().value(QStringLiteral("pin_cpu_threads"), 0).toInt());
    pin_combo->setEnabled(QThread::idealThreadCount() >= 6);
    connect(pin_combo, &QComboBox::currentIndexChanged, this,
            [](int index) { QSettings().setValue(QStringLiteral("pin_cpu_threads"), index); });
    form->addRow(tr("Fijar hilos de CPU (Linux):"), pin_combo);
#endif

#ifdef Q_OS_LINUX
    malloc_check = new QCheckBox(tr("Gestor de memoria más rápido (tcmalloc, si está instalado)"));
    malloc_check->setToolTip(tr("Precarga tcmalloc de gperftools: el renderizador y los ganchos gráficos reservan mucha memoria, y gana varios fps "
                                "en carreras a pantalla partida. Instala el paquete gperftools (libtcmalloc_minimal.so.4) para usarlo."));
    malloc_check->setChecked(QSettings().value(QStringLiteral("use_tcmalloc"), true).toBool());
    connect(malloc_check, &QCheckBox::toggled, this, [](bool on) { QSettings().setValue(QStringLiteral("use_tcmalloc"), on); });
    form->addRow(malloc_check);
#endif
    status_label = new QLabel;
    status_label->setWordWrap(true);
    fullscreen_check = new QCheckBox(tr("Pantalla completa"));
    nvn_capture_check = new QCheckBox(tr("Grabar traza gráfica (desarrollador)"));
    nvn_capture_check->setToolTip(
        tr("Registra las llamadas gráficas NVN del juego en user/log (nvn_trace.txt, nvn_frames.csv y "
           "capturas de dos fotogramas cada 30 s). Sirve para desarrollar la GPU nativa. Más lento."));

    play_button = new QPushButton(tr("Jugar"));
    play_button->setMinimumHeight(40);
    stop_button = new QPushButton(tr("Detener"));
    stop_button->setEnabled(false);
    connect(play_button, &QPushButton::clicked, this, &MainWindow::launch);
    connect(stop_button, &QPushButton::clicked, this, [this] { game.terminate(); });
#ifdef Q_OS_LINUX
    appimage_button = new QPushButton(tr("Empaquetar como AppImage…"));
    appimage_button->setToolTip(tr("Empaqueta el juego construido en este PC en un AppImage para este PC (no lo compartas: contiene el código "
                                   "traducido del juego). Partidas, ajustes y mods pasan a ~/.local/share/kartrecomp y "
                                   "~/.config/kartrecomp; tus archivos del juego, firmware y mods se enlazan allí, no se copian."));
    connect(appimage_button, &QPushButton::clicked, this, &MainWindow::packageAppImage);
#ifdef Q_OS_WIN
    appimage_button->hide();  // AppImages are a Linux thing
#endif
#endif

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(fullscreen_check);
    buttons->addWidget(nvn_capture_check);
    buttons->addStretch();
    auto* update_button = new QPushButton(tr("Buscar actualizaciones"));
    connect(update_button, &QPushButton::clicked, this, [this] { updater->check(false); });
    buttons->addWidget(update_button);
    if (appimage_button) buttons->addWidget(appimage_button);
    buttons->addWidget(stop_button);
    buttons->addWidget(play_button);

    log_view = new QPlainTextEdit;
    log_view->setReadOnly(true);
    log_view->setMaximumBlockCount(2000);

    auto* layout = new QVBoxLayout(page);
    layout->addLayout(form);
    layout->addWidget(status_label);
    layout->addLayout(buttons);
    appimage_progress = new QProgressBar;
    appimage_progress->setRange(0, 3);
    appimage_progress->setFormat(tr("Empaquetando: paso %v de %m"));
    appimage_progress->hide();
    layout->addWidget(appimage_progress);
    layout->addWidget(new QLabel(tr("Salida:")));
    layout->addWidget(log_view, 1);
    return page;
}

QWidget* MainWindow::buildDisplayTab() {
    auto* page = display_page = new QWidget;
    auto* form = new QFormLayout(page);
    resolution_combo = makeCombo(kResolutions);
    aspect_combo = makeCombo(kAspects);
    filter_combo = makeCombo(kFilters);
    aa_combo = makeCombo(kAntiAliasing);
    vsync_combo = makeCombo(kVSync);
    fullscreen_mode_combo = makeCombo(kFullscreenModes);
    docked_combo = makeCombo(kConsoleModes);
    form->addRow(tr("Resolución interna:"), resolution_combo);
    form->addRow(tr("Relación de aspecto:"), aspect_combo);
    form->addRow(tr("Filtro de escalado:"), filter_combo);
    fsr_sharpness_spin = new QSpinBox;
    fsr_sharpness_spin->setRange(0, 200);
    fsr_sharpness_spin->setSuffix(QStringLiteral(" %"));
    form->addRow(tr("Nitidez FSR:"), fsr_sharpness_spin);
    form->addRow(tr("Antialiasing:"), aa_combo);
    form->addRow(tr("Sincronización vertical:"), vsync_combo);
    form->addRow(tr("Modo de pantalla completa:"), fullscreen_mode_combo);
    form->addRow(tr("Calidad gráfica:"), docked_combo);
    auto* note = new QLabel(
        tr("Calidad gráfica: Alta es el modo de consola en base (el juego se dibuja a 1080p con sus mejores efectos), Baja es "
           "el modo portátil (720p, efectos más ligeros). También se cambia dentro del juego (F10). La escala de resolución "
           "multiplica eso, y solo se nota más nítido cuando la ventana o la pantalla es más grande que la resolución base, "
           "por ejemplo 2x a pantalla completa en un monitor 1440p o 4K. AMD FSR (1) con nitidez es el reescalador incluido. "
           "Los cambios se guardan al momento y se aplican desde el siguiente arranque; se bloquean mientras el juego está abierto."));
    note->setWordWrap(true);
    form->addRow(note);
    return page;
}

QWidget* MainWindow::buildFrameRateTab() {
    auto* page = frame_rate_page = new QWidget;
    auto* form = new QFormLayout(page);
    limit_check = new QCheckBox(tr("Limitar la velocidad de emulación"));
    limit_spin = new QSpinBox;
    limit_spin->setRange(10, 400);
    limit_spin->setSuffix(QStringLiteral(" %"));
    connect(limit_check, &QCheckBox::toggled, limit_spin, &QSpinBox::setEnabled);
    form->addRow(limit_check);
    form->addRow(tr("Límite de velocidad:"), limit_spin);
    auto* note = new QLabel(
        tr("100 % son los 60 fps nativos del juego. Otro valor acelera o frena todo el juego. Las tasas de "
           "fotogramas personalizadas de verdad (por ejemplo 120 fps a velocidad normal) necesitan parches "
           "del juego generados a partir de tu propia copia. Está previsto; mira docs/ROADMAP.md."));
    note->setWordWrap(true);
    form->addRow(note);
    return page;
}

QWidget* MainWindow::buildControllersTab() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    auto* note = new QLabel(tr("Controles del jugador 1 según la configuración del juego. Los mandos conectados al "
                               "arrancar se asignan solos. Está prevista una pantalla para "
                               "reasignar botones."));
    note->setWordWrap(true);
    controls_table = new QTableWidget(0, 2);
    controls_table->setHorizontalHeaderLabels({tr("Entrada"), tr("Asignación")});
    controls_table->horizontalHeader()->setStretchLastSection(true);
    controls_table->verticalHeader()->hide();
    controls_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    players_table = new QTableWidget(4, 3);
    players_table->setHorizontalHeaderLabels({tr("Jugador"), tr("Estado"), tr("Asignado a")});
    players_table->horizontalHeader()->setStretchLastSection(true);
    players_table->verticalHeader()->hide();
    players_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    players_table->setMaximumHeight(150);
    auto* assign_note = new QLabel(tr("Jugadores 1 a 4. Los mandos se reparten solos a medida que se conectan. Para elegir quién juega con qué "
                                      "(un mando, el teclado o nadie), abre el menú del juego con F10, o pulsando los dos sticks "
                                      "en un mando, y usa las filas \"Mando jugador 1\" a \"Mando jugador 4\"."));
    assign_note->setWordWrap(true);
    layout->addWidget(note);
    layout->addWidget(assign_note);
    layout->addWidget(players_table);
    layout->addWidget(controls_table, 1);
    return page;
}

QWidget* MainWindow::buildMiiTab() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    auto* note = new QLabel(
        tr("Está previsto poder crear Miis a mano. Los Miis se guardan en la partida del sistema 8000000000000030 "
           "(MiiDatabase.dat), dentro de la carpeta user/nand del juego. El editor escribirá ahí registros "
           "válidos (nombre, cara, pelo, colores) para que aparezcan en la lista de pilotos Mii del juego."));
    note->setWordWrap(true);
    layout->addWidget(note);
    layout->addStretch();
    return page;
}

QWidget* MainWindow::buildDlcTab() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    auto* note = new QLabel(tr("Contenido descargable (NSP) de tu propio volcado. Cada uno se pasa al "
                               "juego con --content-dlc. Los DLC son solo datos: no hace falta reconstruir el juego."));
    note->setWordWrap(true);
    dlc_list = new QListWidget;
    auto* add = new QPushButton(tr("Añadir DLC…"));
    auto* remove = new QPushButton(tr("Quitar los seleccionados"));
    connect(add, &QPushButton::clicked, this, [this] {
        const auto files = QFileDialog::getOpenFileNames(this, tr("Añadir DLC"), {},
                                                         tr("Archivos NSP (*.nsp)"));
        Portable::EnterAppDir();
        QDir().mkpath(package.dlcDir());
        for (const auto& f : files) {
            const QString target = QDir(package.dlcDir()).filePath(QFileInfo(f).fileName());
            // A hard link costs no space when the file is on the same drive; copy otherwise.
            if (!QFile::exists(target) && !QFile::link(f, target) && !QFile::copy(f, target)) {
                QMessageBox::warning(this, tr("DLC"), tr("No se pudo añadir %1").arg(f));
            }
        }
        refreshDlc();
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        for (auto* item : dlc_list->selectedItems()) {
            QFile::remove(QDir(package.dlcDir()).filePath(item->text()));
        }
        refreshDlc();
    });
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(add);
    buttons->addWidget(remove);
    buttons->addStretch();
    layout->addWidget(note);
    layout->addWidget(dlc_list, 1);
    layout->addLayout(buttons);
    return page;
}

void MainWindow::reloadPackage() {
    if (!package.isInstalled()) {
        package.setRoot(root_edit->text().trimmed());
        package.setSuyuHome(home_edit->text().trimmed());
        QSettings prefs;
        prefs.setValue(QStringLiteral("game_root"), package.root());
        prefs.setValue(QStringLiteral("suyu_home"), package.suyuHome());
    }

    const QString problems = package.problems();
    status_label->setText(problems.isEmpty() ? tr("Listo.") : problems);
    play_button->setEnabled(problems.isEmpty() && game.state() == QProcess::NotRunning);
    loadSettings();
    refreshDlc();
    refreshControllers();
    loadOverlay();
    refreshMods();
}

void MainWindow::loadSettings() {
    const QSignalBlocker guard(this);
    loading_settings = true;
    config.load(package.configFile());
    // Defaults match suyu's own when the game has not written a config yet.
    resolution_combo->setCurrentIndex(config.intValue(kRenderer, QStringLiteral("resolution_setup"), 3));
    aspect_combo->setCurrentIndex(config.intValue(kRenderer, QStringLiteral("aspect_ratio"), 0));
    filter_combo->setCurrentIndex(config.intValue(kRenderer, QStringLiteral("scaling_filter"), 1));
    aa_combo->setCurrentIndex(config.intValue(kRenderer, QStringLiteral("anti_aliasing"), 0));
    vsync_combo->setCurrentIndex(config.intValue(kRenderer, QStringLiteral("use_vsync"), 2));
    fullscreen_mode_combo->setCurrentIndex(
        config.intValue(kRenderer, QStringLiteral("fullscreen_mode"), 0));
    docked_combo->setCurrentIndex(config.intValue(kSystem, QStringLiteral("use_docked_mode"), 1));
    {
        // Index order is suyu's `Language` enum. A config still on suyu's default (\default=true) counts as no choice: Spanish (Spain).
        const bool chosen = config.value(kSystem, QStringLiteral("language_index\\default")) == QLatin1String("false");
        language_combo->setCurrentIndex(chosen ? config.intValue(kSystem, QStringLiteral("language_index"), 5) : 5);
    }
    limit_check->setChecked(config.boolValue(kCore, QStringLiteral("use_speed_limit"), true));
    limit_spin->setValue(config.intValue(kCore, QStringLiteral("speed_limit"), 100));
    limit_spin->setEnabled(limit_check->isChecked());
    // Stored as the slider position: suyu's default is 25 (0 = off, 200 = maximum).
    fsr_sharpness_spin->setValue(config.intValue(kRenderer, QStringLiteral("fsr_sharpening_slider"), 25));
    fsr_sharpness_spin->setEnabled(filter_combo->currentIndex() == kFsrFilterIndex);
    loading_settings = false;
}

bool MainWindow::saveSettings() {
    if (!config.isLoaded()) {
        return true; // first launch: nothing to edit yet, the game writes its defaults
    }
    config.setInt(kRenderer, QStringLiteral("resolution_setup"), resolution_combo->currentIndex());
    config.setInt(kRenderer, QStringLiteral("aspect_ratio"), aspect_combo->currentIndex());
    config.setInt(kRenderer, QStringLiteral("scaling_filter"), filter_combo->currentIndex());
    config.setInt(kRenderer, QStringLiteral("anti_aliasing"), aa_combo->currentIndex());
    config.setInt(kRenderer, QStringLiteral("use_vsync"), vsync_combo->currentIndex());
    config.setInt(kRenderer, QStringLiteral("fullscreen_mode"), fullscreen_mode_combo->currentIndex());
    config.setInt(kSystem, QStringLiteral("use_docked_mode"), docked_combo->currentIndex());
    // Read by the engine at every boot (ISettingsServer::GetLanguageCode): the console language until changed here or in game.
    config.setInt(kSystem, QStringLiteral("language_index"), language_combo->currentIndex());
    // A region that goes with it (suyu's Region: 0 Japan, 1 USA, 2 Europe); Chinese, Korean and Thai keep the config's own.
    static const int kRegionFor[] = {0, 1, 2, 2, 2, 2, -1, -1, 2, 2, 2, -1, 2, 1, 1, -1, -1, 1, 2, -1};
    if (const int l = language_combo->currentIndex(); l >= 0 && l < static_cast<int>(std::size(kRegionFor)) && kRegionFor[l] >= 0)
        config.setInt(kSystem, QStringLiteral("region_index"), kRegionFor[l]);
    config.setBool(kCore, QStringLiteral("use_speed_limit"), limit_check->isChecked());
    config.setInt(kCore, QStringLiteral("speed_limit"), limit_spin->value());
    config.setInt(kRenderer, QStringLiteral("fsr_sharpening_slider"), fsr_sharpness_spin->value());
    return config.save();
}

void MainWindow::applySettingsNow() {
    if (loading_settings || game.state() != QProcess::NotRunning) {
        return;
    }
    fsr_sharpness_spin->setEnabled(filter_combo->currentIndex() == kFsrFilterIndex);
    // Re-read first so anything the game wrote since we loaded is kept.
    IniFile fresh;
    if (!fresh.load(package.configFile())) {
        status_label->setText(tr("Abre el juego una vez para que cree su configuración; después "
                                 "podrás cambiar los ajustes."));
        return;
    }
    config = fresh;
    if (!saveSettings()) {
        status_label->setText(tr("No se pudo escribir %1").arg(package.configFile()));
    }
}

void MainWindow::connectAutoSave() {
    for (auto* combo : {resolution_combo, aspect_combo, filter_combo, aa_combo, vsync_combo,
                        fullscreen_mode_combo, docked_combo, language_combo}) {
        connect(combo, &QComboBox::currentIndexChanged, this, &MainWindow::applySettingsNow);
    }
    connect(limit_check, &QCheckBox::toggled, this, &MainWindow::applySettingsNow);
    connect(limit_spin, &QSpinBox::valueChanged, this, &MainWindow::applySettingsNow);
    connect(fsr_sharpness_spin, &QSpinBox::valueChanged, this, &MainWindow::applySettingsNow);
}

void MainWindow::refreshDlc() {
    dlc_list->clear();
    for (const auto& f : package.dlcFiles()) {
        dlc_list->addItem(QFileInfo(f).fileName());
    }
}

void MainWindow::refreshControllers() {
    for (int p = 0; p < 4; ++p) {
        const QString prefix = QStringLiteral("player_%1_").arg(p);
        const bool connected = config.boolValue(kControls, prefix + QStringLiteral("connected"), p == 0);
        const QString a = config.value(kControls, prefix + QStringLiteral("button_a"));
        QString device = tr("nada");
        if (a.contains(QLatin1String("engine:keyboard"))) device = tr("teclado");
        else if (a.contains(QLatin1String("engine:sdl"))) device = tr("mando");
        else if (a.contains(QLatin1String("engine:joycon"))) device = tr("Joy-Con");
        players_table->setItem(p, 0, new QTableWidgetItem(tr("Jugador %1").arg(p + 1)));
        players_table->setItem(p, 1, new QTableWidgetItem(connected ? tr("conectado") : tr("no conectado")));
        players_table->setItem(p, 2, new QTableWidgetItem(device));
    }
    controls_table->setRowCount(0);
    const auto binds = config.keysWithPrefix(kControls, QStringLiteral("player_0_"));
    for (const auto& [key, value] : binds) {
        if (!key.contains(QLatin1String("button_")) && !key.contains(QLatin1String("stick"))) {
            continue;
        }
        const int row = controls_table->rowCount();
        controls_table->insertRow(row);
        controls_table->setItem(row, 0, new QTableWidgetItem(key.mid(9)));
        controls_table->setItem(row, 1, new QTableWidgetItem(value));
    }
}

QProcessEnvironment MainWindow::launchEnvironment() const {
    auto env = package.launchEnvironment();
    if (renderer_combo->currentIndex() == 1) {
        env.insert(QStringLiteral("KARTRECOMP_NATIVE_GPU"), QStringLiteral("native"));
        static const int kHeights[] = {720, 1080, 1440, 2160};
        env.insert(QStringLiteral("KARTRECOMP_NATIVE_HEIGHT"),
                   QString::number(kHeights[qBound(0, native_res_combo->currentIndex(), 3)]));
    }
#ifdef Q_OS_LINUX
    if (pin_combo && pin_combo->currentIndex() == 1 && QThread::idealThreadCount() >= 6) {
        // Guest cores 0-3 on four CPUs and the render thread on the next, leaving CPUs 0-1 (or 0) free
        // for the desktop, audio and the rest of the system.
        const int first = QThread::idealThreadCount() >= 8 ? 2 : 1;
        env.insert(QStringLiteral("KARTRECOMP_PIN"),
                   QStringLiteral("%1,%2,%3,%4").arg(first).arg(first + 1).arg(first + 2).arg(first + 3));
        env.insert(QStringLiteral("KARTRECOMP_PIN_RENDER"), QString::number(first + 4));
    }
#endif
#ifdef Q_OS_LINUX
    if (malloc_check && malloc_check->isChecked()) {
        for (const QString& lib : {QStringLiteral("/usr/lib/libtcmalloc_minimal.so.4"), QStringLiteral("/usr/lib64/libtcmalloc_minimal.so.4"),
                                   QStringLiteral("/usr/lib/x86_64-linux-gnu/libtcmalloc_minimal.so.4")}) {
            if (QFileInfo::exists(lib)) {
                env.insert(QStringLiteral("LD_PRELOAD"), lib);
                break;
            }
        }
    }
#endif
    if (nvn_capture_check->isChecked()) {
        env.insert(QStringLiteral("KARTRECOMP_NVN_TRACE"), QStringLiteral("1"));
        env.insert(QStringLiteral("KARTRECOMP_NVN_CAPTURE"), QStringLiteral("every=1800,frames=2"));
    }
    return env;
}

bool MainWindow::launchWithoutWindow() {
    const QString problems = package.problems();
    if (!problems.isEmpty()) {
        QMessageBox::warning(nullptr, tr("No se puede iniciar"), problems + QStringLiteral("\n\n") + tr("Abre el launcher sin --nolauncher para arreglarlo."));
        return false;
    }
    connect(&game, &QProcess::finished, qApp, [](int code, QProcess::ExitStatus) { QCoreApplication::exit(code); });
    connect(&game, &QProcess::errorOccurred, qApp, [](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) QCoreApplication::exit(1);
    });
    launch();
    return game.state() != QProcess::NotRunning; // start() is asynchronous: Starting counts
}

void MainWindow::launch() {
    const QString problems = package.problems();
    if (!problems.isEmpty()) {
        QMessageBox::warning(this, tr("No se puede iniciar"), problems);
        return;
    }
    // Settings are re-read first so edits made by the game since the last load survive.
    IniFile fresh;
    if (fresh.load(package.configFile())) {
        config = fresh;
    }
    if (!package.fixDataPaths() || !saveSettings()) {
        QMessageBox::warning(this, tr("Ajustes"), tr("No se pudo escribir %1").arg(package.configFile()));
        return;
    }
    log_view->clear();
    if (!resolveCtgpConflict(false, false)) return;
    // Menu archives from mods are made for one update (nearly always 3.0.x); on 4.0.1 the game stalls on them at its first loading screen.
    if (!package.root().isEmpty()) {
        QFile info(QDir(package.root()).filePath(QStringLiteral("module/module.json")));
        const bool is401 = info.open(QIODevice::ReadOnly) && info.readAll().contains("\"update\": \"4.0.1\"");
        const QStringList menus = is401 ? ModAddon::ModsReplacingMenus(package.root()) : QStringList();
        if (!menus.isEmpty()) {
            const auto answer = QMessageBox::question(this, tr("Mods hechos para la 3.0.4"),
                tr("Tu juego está construido con la actualización 4.0.1, y estos mods sustituyen los archivos de menús del juego por versiones "
                   "hechas para una actualización anterior. Con ellos el juego se queda en la primera pantalla de carga.\n\n%1\n\n¿Desactivarlos e iniciar?").arg(menus.join(QLatin1Char('\n'))),
                QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
            if (answer == QMessageBox::Cancel) return;
            if (answer == QMessageBox::Yes) {
                for (const auto& name : menus) ModAddon::SetModEnabled(package.root(), name, false, nullptr);
                refreshMods();
            }
        }
    }
    // Merged mod texts are made from the game's own texts, which differ between updates (3.0.4 / 4.0.1): texts merged for another update
    // stall the game at its first loading screen, so they are re-merged first.
    if (!package.root().isEmpty() && ModAddon::NeedsTextMerge(package.root())) {
        syncTextMerge();
        status_label->setText(tr("Actualizando los textos combinados de los mods para esta versión del juego; pulsa Jugar otra vez cuando termine."));
        return;
    }
    game.setWorkingDirectory(package.workingDirectory());
    auto env = launchEnvironment();
    game.setProcessEnvironment(env);
    game.setProcessChannelMode(QProcess::MergedChannels);
    game.start(package.executable(), package.launchArguments(fullscreen_check->isChecked()));
    status_label->setText(tr("En marcha…"));
}


#ifdef Q_OS_LINUX
// The AppImage keeps nothing of the user's in its own (read-only) folder: data in $XDG_DATA_HOME/kartrecomp, settings in
// $XDG_CONFIG_HOME/kartrecomp (see packaging/AppRun). This fills those from the game folder the first time (links for the big
// files, copies for saves and settings), writes the launcher's choices to launch.env, then runs packaging/make-appimage.sh.
void MainWindow::packageAppImage() {
    const QString app_dir = QCoreApplication::applicationDirPath();
    QString script;
    for (const QString& c : {QStringLiteral("../../packaging/make-appimage.sh"), QStringLiteral("../packaging/make-appimage.sh"),
                             QStringLiteral("packaging/make-appimage.sh")}) {
        if (QFileInfo::exists(QDir(app_dir).filePath(c))) {
            script = QDir(app_dir).absoluteFilePath(c);
            break;
        }
    }
    if (script.isEmpty() || !QFileInfo::exists(package.executable())) {
        QMessageBox::warning(this, tr("AppImage"), tr("Hace falta el juego construido (%1) y packaging/make-appimage.sh.").arg(package.executable()));
        return;
    }
    const QString home = QDir::homePath();
    const QString out = QFileDialog::getExistingDirectory(this, tr("¿Dónde guardo el AppImage?"), QDir(home).filePath(QStringLiteral("Applications")));
    if (out.isEmpty()) return;
    const auto xdg = [&](const char* var, const char* fallback) {
        const QString v = qEnvironmentVariable(var);
        return QDir(v.isEmpty() ? QDir(home).filePath(QLatin1String(fallback)) : v).filePath(QStringLiteral("kartrecomp"));
    };
    const QDir data(xdg("XDG_DATA_HOME", ".local/share")), conf(xdg("XDG_CONFIG_HOME", ".config"));
    const QDir game_dir(package.root());
    QDir().mkpath(data.filePath(QStringLiteral("content/dlc")));
    QDir().mkpath(conf.path());
    const auto link = [](const QString& target, const QString& at) {
        if (QFileInfo::exists(target) && !QFileInfo(at).exists() && !QFileInfo(at).isSymLink()) QFile::link(target, at);
    };
    link(package.baseNsp(), data.filePath(QStringLiteral("content/base.nsp")));
    link(package.updateNsp(), data.filePath(QStringLiteral("content/update-3.0.4.nsp")));
    for (const auto& dlc : QDir(package.dlcDir()).entryInfoList({QStringLiteral("*.nsp")}, QDir::Files)) {
        link(dlc.absoluteFilePath(), data.filePath(QStringLiteral("content/dlc/") + dlc.fileName()));
    }
    link(game_dir.filePath(QStringLiteral("mods")), data.filePath(QStringLiteral("mods")));
    // Saves (and the rest of the emulated NAND) are copied once; the firmware next to them is linked from the installed suyu.
    const QDir suyu(QDir(package.suyuHome()).filePath(QStringLiteral("suyu")));
    if (!data.exists(QStringLiteral("nand"))) {
        QProcess::execute(QStringLiteral("cp"), {QStringLiteral("-a"), game_dir.filePath(QStringLiteral("user/nand")), data.filePath(QStringLiteral("nand"))});
        const QString fw = data.filePath(QStringLiteral("nand/system/Contents"));
        if (QFileInfo::exists(suyu.filePath(QStringLiteral("nand/system/Contents")))) {
            QDir(fw).removeRecursively(); // the game folder's copy has no firmware in it
            QFile::link(suyu.filePath(QStringLiteral("nand/system/Contents")), fw);
        }
    }
    if (!data.exists(QStringLiteral("keys/prod.keys")) && suyu.exists(QStringLiteral("keys"))) {
        QProcess::execute(QStringLiteral("cp"), {QStringLiteral("-a"), suyu.filePath(QStringLiteral("keys")), data.filePath(QStringLiteral("keys"))});
    }
    for (const QString& f : {QStringLiteral("sdl2-config.ini"), QStringLiteral("kartrecomp_overlay.ini")}) {
        if (!conf.exists(f)) QFile::copy(game_dir.filePath(QStringLiteral("user/config/") + f), conf.filePath(f));
    }
    // The launcher's renderer / resolution / pinning choices, read by AppRun.
    QFile env_file(conf.filePath(QStringLiteral("launch.env")));
    if (env_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        const auto env = launchEnvironment();
        for (const QString& key : env.keys()) {
            if (key.startsWith(QLatin1String("KARTRECOMP_"))) env_file.write((key + QLatin1Char('=') + env.value(key) + QLatin1Char('\n')).toUtf8());
        }
    }
    log_view->clear();
    appimage_button->setEnabled(false);
    status_label->setText(tr("Empaquetando…"));
    appimage_progress->setValue(0);
    appimage_progress->show();
    auto* proc = new QProcess(this);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    // make-appimage.sh prints "[n/3] what it is doing" before each step.
    connect(proc, &QProcess::readyRead, this, [this, proc] {
        const QString text = QString::fromUtf8(proc->readAll()).trimmed();
        log_view->appendPlainText(text);
        static const QRegularExpression step(QStringLiteral("\\[(\\d)/3\\] ([^\\n]*)"));
        for (auto it = step.globalMatch(text); it.hasNext();) {
            const auto m = it.next();
            appimage_progress->setValue(m.captured(1).toInt() - 1);
            status_label->setText(tr("Empaquetando: %1…").arg(m.captured(2)));
        }
    });
    connect(proc, &QProcess::finished, this, [this, proc, out](int code) {
        appimage_button->setEnabled(true);
        appimage_progress->setValue(code == 0 ? 3 : appimage_progress->value());
        if (code != 0) appimage_progress->hide();
        status_label->setText(code == 0 ? tr("AppImage guardado en %1").arg(QDir(out).filePath(QStringLiteral("KartRecomp-x86_64.AppImage")))
                                        : tr("Falló el empaquetado (mira la salida)."));
        proc->deleteLater();
    });
    proc->start(QStringLiteral("bash"), {script, package.root(), out});
}
#endif

QWidget* MainWindow::buildEnhancementsTab() {
    auto* page = enhancements_page = new QWidget;
    auto* form = new QFormLayout(page);

    upscaler_combo = new QComboBox;
    upscaler_combo->setToolTip(tr("Reescala (o, al 100%, suaviza los bordes de) la imagen 3D; el HUD nunca se reescala. FSR 3 y DLSS son complementos "
                                  "de la carpeta plugins del juego; DLSS necesita una GPU NVIDIA."));
    scale_combo = makeCombo({tr("Desactivado (3D a la resolución de salida)"), tr("100% (DLAA / antialiasing nativo)"), tr("67% (calidad)"),
                             tr("58% (equilibrado)"), tr("50% (rendimiento)"), tr("33% (ultra rendimiento)"), tr("150% (supermuestreo)"),
                             tr("200% (supermuestreo)")});
    scale_combo->setToolTip(tr("A qué tamaño se dibuja la escena 3D respecto a la resolución de salida. Por debajo del 100% hace falta un reescalador temporal."));
    sharpness_combo = makeCombo({tr("Desactivada"), tr("Baja"), tr("Media"), tr("Alta")});
    jitter_check = new QCheckBox(tr("Vibración de cámara (jitter; la necesitan los reescaladores temporales, que la activan solos)"));
    framegen_combo = new QComboBox;
    framegen_combo->setToolTip(tr("Duplica los fotogramas generando uno entre cada par de fotogramas reales. El HUD se dibuja encima "
                                  "del fotograma generado, así que nunca se emborrona. Necesita un reescalador temporal."));
    hdr_check = new QCheckBox(tr("Salida HDR10 (la pantalla debe estar en modo HDR)"));
    hdr_paper_combo = makeCombo({tr("100 nits"), tr("150 nits"), tr("203 nits (estándar)"), tr("250 nits"), tr("300 nits")});
    hdr_peak_combo = makeCombo({tr("400 nits"), tr("600 nits"), tr("800 nits"), tr("1000 nits"), tr("1500 nits")});
    present_combo = makeCombo({tr("Mailbox"), tr("FIFO (vsync)"), tr("Inmediato")});
    fps_check = new QCheckBox(tr("Mostrar contador de FPS"));

    form->addRow(tr("Reescalador:"), upscaler_combo);
#ifdef Q_OS_LINUX
    dlss_button = new QPushButton(tr("Configurar NVIDIA DLSS..."));
    dlss_button->setToolTip(tr("Para GPU NVIDIA. Descarga el SDK de DLSS de NVIDIA en este PC y compila aquí un pequeño complemento, porque los "
                               "archivos de NVIDIA no pueden distribuirse con el juego. Necesita un compilador de C++ y las cabeceras de Vulkan."));
    connect(dlss_button, &QPushButton::clicked, this, &MainWindow::setupDlss);
    form->addRow(QString(), dlss_button);
#endif
    form->addRow(tr("Escala de renderizado 3D:"), scale_combo);
    form->addRow(tr("Nitidez:"), sharpness_combo);
    form->addRow(jitter_check);
    form->addRow(tr("Generación de fotogramas:"), framegen_combo);
    form->addRow(hdr_check);
    form->addRow(tr("Blanco de papel HDR:"), hdr_paper_combo);
    form->addRow(tr("Brillo máximo HDR:"), hdr_peak_combo);
    form->addRow(tr("Modo de presentación:"), present_combo);
    form->addRow(fps_check);
    auto* note = new QLabel(
        tr("Son los mismos ajustes que el menú del juego (F10, o pulsando los dos sticks en un mando) y se comparten: un cambio dentro "
           "del juego se guarda aquí, y estos valores son con los que arranca el juego. Solo se aplican al renderizador Vulkan nativo y se "
           "bloquean mientras el juego está abierto."));
    note->setWordWrap(true);
    form->addRow(note);

    const auto save = [this] { saveOverlay(); };
    for (auto* combo : {upscaler_combo, scale_combo, sharpness_combo, framegen_combo, hdr_paper_combo, hdr_peak_combo, present_combo}) {
        connect(combo, &QComboBox::currentIndexChanged, this, save);
    }
    for (auto* check : {jitter_check, hdr_check, fps_check}) {
        connect(check, &QCheckBox::toggled, this, save);
    }
    connect(hdr_check, &QCheckBox::toggled, this, [this](bool on) {
        hdr_paper_combo->setEnabled(on);
        hdr_peak_combo->setEnabled(on);
    });
    return page;
}

void MainWindow::setupDlss() {
#ifdef Q_OS_LINUX
    const QString app = QCoreApplication::applicationDirPath();
    QString src;
    for (const QString& c : {app + "/../lib/kartrecomp/plugin-src/dlss", app + "/../../plugins/dlss", app + "/plugins/dlss"}) {
        if (QFileInfo::exists(c + "/install-linux.sh") && QFileInfo::exists(c + "/kr_upscaler_dlss.cpp")) {
            src = QDir::cleanPath(c);
            break;
        }
    }
    if (src.isEmpty()) {
        QMessageBox::warning(this, tr("NVIDIA DLSS"), tr("El código del complemento DLSS no está en esta instalación."));
        return;
    }
    const QString dest = DataLayout::Default().pluginsDir();
    if (QMessageBox::question(this, tr("Configurar NVIDIA DLSS"),
                              tr("Esto descarga el SDK de DLSS de NVIDIA (github.com/NVIDIA/DLSS) en este PC, compila un pequeño complemento con tu "
                                 "compilador de C++ y lo instala en:\n%1\n\nLos archivos de NVIDIA se quedan en este PC; nunca forman parte de la descarga del juego. "
                                 "Funciona en GPU NVIDIA con un controlador actual. ¿Continuar?").arg(dest)) != QMessageBox::Yes) {
        return;
    }
    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Configurar NVIDIA DLSS"));
    dialog->resize(720, 420);
    auto* lay = new QVBoxLayout(dialog);
    auto* log = new QPlainTextEdit;
    log->setReadOnly(true);
    lay->addWidget(log, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    buttons->button(QDialogButtonBox::Close)->setEnabled(false);
    lay->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    auto* proc = new QProcess(dialog);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    connect(proc, &QProcess::readyRead, log, [proc, log] { log->appendPlainText(QString::fromLocal8Bit(proc->readAll()).trimmed()); });
    connect(proc, &QProcess::finished, dialog, [this, proc, log, buttons](int code, QProcess::ExitStatus) {
        log->appendPlainText(code == 0 ? tr("\nTerminado.") : tr("\nHa fallado (código %1). Corrige lo que aparece arriba y vuelve a intentarlo.").arg(code));
        buttons->button(QDialogButtonBox::Close)->setEnabled(true);
        if (code == 0) loadOverlay();
    });
    connect(proc, &QProcess::errorOccurred, dialog, [log, buttons](QProcess::ProcessError) {
        log->appendPlainText(tr("No se pudo iniciar el script de instalación (¿está instalado bash?)."));
        buttons->button(QDialogButtonBox::Close)->setEnabled(true);
    });
    dialog->show();
    proc->start(QStringLiteral("bash"), {src + "/install-linux.sh", src, dest});
#endif
}

void MainWindow::loadOverlay() {
    loading_settings = true;
    const QSignalBlocker b1(upscaler_combo), b2(scale_combo), b3(sharpness_combo), b4(framegen_combo), b5(hdr_paper_combo), b6(hdr_peak_combo),
        b7(present_combo), b8(jitter_check), b9(hdr_check), b10(fps_check);
    overlay.load(package.overlayFile());

    upscaler_ids = QStringList{QStringLiteral("off"), QStringLiteral("fsr1")} + FindPluginIds(package.pluginsParent(), QStringLiteral("upscaler"), {DataLayout::Default().pluginsDir()});
    upscaler_combo->clear();
    for (const auto& id : upscaler_ids) {
        upscaler_combo->addItem(id == QLatin1String("off")    ? tr("Desactivado")
                                : id == QLatin1String("fsr1") ? tr("FSR 1 (espacial)")
                                : id == QLatin1String("fsr3") ? tr("FSR 3.1 (temporal)")
                                : id == QLatin1String("dlss") ? tr("NVIDIA DLSS")
                                                              : id);
    }
    upscaler_combo->setCurrentIndex(qMax(0, upscaler_ids.indexOf(overlay.value(QStringLiteral("upscaler"), QStringLiteral("off")))));

    framegen_ids = QStringList{QStringLiteral("off"), QStringLiteral("builtin")} + FindPluginIds(package.pluginsParent(), QStringLiteral("framegen"), {DataLayout::Default().pluginsDir()});
    if (!framegen_ids.contains(QStringLiteral("fsr3")) && FindPluginIds(package.pluginsParent(), QStringLiteral("upscaler"), {DataLayout::Default().pluginsDir()}).contains(QStringLiteral("fsr3"))) {
        framegen_ids << QStringLiteral("fsr3"); // the FSR 3 library carries its frame generation
    }
    framegen_combo->clear();
    for (const auto& id : framegen_ids) {
        framegen_combo->addItem(id == QLatin1String("off")       ? tr("Desactivada")
                                : id == QLatin1String("builtin") ? tr("Integrada (2x)")
                                : id == QLatin1String("fsr3")    ? tr("Generación de fotogramas FSR 3 (2x)")
                                                                 : id + QStringLiteral(" (2x)"));
    }
    framegen_combo->setCurrentIndex(qMax(0, framegen_ids.indexOf(overlay.value(QStringLiteral("framegen"), QStringLiteral("off")))));

    scale_combo->setCurrentIndex(qBound(0, overlay.intValue(QStringLiteral("quality"), 0), scale_combo->count() - 1));
    sharpness_combo->setCurrentIndex(qBound(0, overlay.intValue(QStringLiteral("sharpness"), 3), 3));
    jitter_check->setChecked(overlay.intValue(QStringLiteral("jitter"), 0) != 0);
    hdr_check->setChecked(overlay.intValue(QStringLiteral("hdr"), 0) != 0);
    hdr_paper_combo->setCurrentIndex(qBound(0, overlay.intValue(QStringLiteral("hdr_paper"), 1), 4));
    hdr_peak_combo->setCurrentIndex(qBound(0, overlay.intValue(QStringLiteral("hdr_peak"), 0), 4));
    hdr_paper_combo->setEnabled(hdr_check->isChecked());
    hdr_peak_combo->setEnabled(hdr_check->isChecked());
    present_combo->setCurrentIndex(qBound(0, overlay.intValue(QStringLiteral("present"), 0), 2));
    fps_check->setChecked(overlay.intValue(QStringLiteral("fps"), 0) != 0);
    loading_settings = false;
}

void MainWindow::saveOverlay() {
    if (loading_settings || game.state() != QProcess::NotRunning || package.root().isEmpty()) return;
    // Re-read first, so anything the game saved since (the speed limit, the resolution rows) is kept.
    overlay.load(package.overlayFile());
    overlay.setValue(QStringLiteral("upscaler"), upscaler_ids.value(upscaler_combo->currentIndex(), QStringLiteral("off")));
    overlay.setValue(QStringLiteral("framegen"), framegen_ids.value(framegen_combo->currentIndex(), QStringLiteral("off")));
    overlay.setInt(QStringLiteral("quality"), scale_combo->currentIndex());
    overlay.setInt(QStringLiteral("sharpness"), sharpness_combo->currentIndex());
    overlay.setInt(QStringLiteral("jitter"), jitter_check->isChecked() ? 1 : 0);
    overlay.setInt(QStringLiteral("hdr"), hdr_check->isChecked() ? 1 : 0);
    overlay.setInt(QStringLiteral("hdr_paper"), hdr_paper_combo->currentIndex());
    overlay.setInt(QStringLiteral("hdr_peak"), hdr_peak_combo->currentIndex());
    overlay.setInt(QStringLiteral("present"), present_combo->currentIndex());
    overlay.setInt(QStringLiteral("fps"), fps_check->isChecked() ? 1 : 0);
    if (!overlay.save()) {
        status_label->setText(tr("No se pudo escribir %1").arg(overlay.path()));
    }
}

QWidget* MainWindow::buildModsTab() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    auto* note = new QLabel(tr("Mods para el juego. Funcionan los mods de datos normales de Mario Kart 8 Deluxe: una carpeta o un zip con una carpeta romfs (y exefs), "
                               "con las estructuras habituales (simple, atmosphere/contents/... de Atmosphere o load/... de yuzu). Sus archivos se ponen por encima "
                               "de los del juego. Los parches de código (exefs, IPS) no se pueden aplicar a esta versión recompilada y se marcan. Todo "
                               "se guarda dentro de la carpeta del juego."));
    note->setWordWrap(true);
    mods_list = new QListWidget;
    mod_details = new QLabel;
    mod_details->setWordWrap(true);
    mod_details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    mod_progress = new QProgressBar;
    mod_progress->hide();

    mod_import_button = new QPushButton(tr("Convertir zip de CTGP DX…"));
    mod_add_folder_button = new QPushButton(tr("Añadir carpeta de mod…"));
    mod_add_zip_button = new QPushButton(tr("Añadir zip de mod…"));
    mod_toggle_button = new QPushButton(tr("Desactivar"));
    mod_remove_button = new QPushButton(tr("Quitar"));
    mod_recomp_button = new QPushButton(tr("Sobre esta conversión"));
    connect(mods_list, &QListWidget::currentRowChanged, this, [this] { refreshMods(); });
    // Each mod has its own check box: ticking turns it on, clearing turns it off (the folder is renamed, nothing is deleted).
    connect(mods_list, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        if (importing || game.state() != QProcess::NotRunning) return;
        const int row = mods_list->row(item);
        const bool want = item->checkState() == Qt::Checked;
        QString error;
        bool ok = true;
        // CTGP's extra cups only run with every other mod off (modded drivers, karts and tracks crash races beside them):
        // turning one side on turns the other side off, after asking.
        if (want && !resolveCtgpConflict(row == 0, row != 0)) {
            const QSignalBlocker block(mods_list);
            item->setCheckState(Qt::Unchecked);
            return;
        }
        if (row == 0) {
            if (ModAddon::CtgpState(package.root()) != ModAddon::State::NotImported) ok = ModAddon::SetCtgpEnabled(package.root(), want, &error);
        } else if (row - 1 < mod_entries.size() && mod_entries[row - 1].enabled != want) {
            ok = ModAddon::SetModEnabled(package.root(), mod_entries[row - 1].name, want, &error);
        }
        if (!ok) QMessageBox::warning(this, tr("Mods"), error);
        refreshMods();
        syncTextMerge();
    });
    connect(mod_import_button, &QPushButton::clicked, this, &MainWindow::importCtgp);
    connect(mod_add_folder_button, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Elige la carpeta de un mod (o una carpeta con varios mods)"));
        Portable::EnterAppDir();
        if (dir.isEmpty()) return;
        // A folder that is itself a mod, or a folder holding several mods (one per sub-folder).
        QStringList sources;
        const QDir d(dir);
        const bool is_mod = QFileInfo(d.filePath(QStringLiteral("romfs"))).isDir() || QFileInfo(d.filePath(QStringLiteral("exefs"))).isDir() ||
                            QFileInfo(d.filePath(QStringLiteral("atmosphere"))).isDir();
        if (is_mod) sources << dir;
        else for (const auto& sub : d.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) sources << d.filePath(sub);
        const QString root = package.root();
        runImport([sources, root](QString* error, const std::function<bool(int, int, const QString&)>& progress) {
            bool any = false;
            for (const auto& s : sources) {
                QString e;
                if (ModAddon::ImportModFolder(s, root, &e, progress)) any = true;
                else if (!e.contains(QLatin1String("No hay carpeta romfs ni exefs")) && error) *error += e + QLatin1Char('\n'); // sub-folders that are not mods stay quiet
            }
            if (!any && error && error->isEmpty()) *error = QStringLiteral("No hay mods en esa carpeta.");
            return any || (error && !error->isEmpty());
        });
    });
    connect(mod_add_zip_button, &QPushButton::clicked, this, [this] {
        const QStringList zips = QFileDialog::getOpenFileNames(this, tr("Elige los zip de mods"), {}, tr("Archivos zip (*.zip)"));
        Portable::EnterAppDir();
        if (zips.isEmpty()) return;
        const QString root = package.root();
        runImport([zips, root](QString* error, const std::function<bool(int, int, const QString&)>& progress) {
            bool any = false;
            for (const auto& z : zips) {
                QString e;
                if (ModAddon::ImportModZip(z, root, &e, progress)) any = true;
                else if (error) *error += QFileInfo(z).fileName() + QStringLiteral(": ") + e + QLatin1Char('\n');
            }
            return any;
        });
    });
    connect(mod_toggle_button, &QPushButton::clicked, this, [this] {
        QString error;
        const int row = mods_list->currentRow();
        bool ok = true;
        if (row <= 0) {
            const bool enable = ModAddon::CtgpState(package.root()) == ModAddon::State::Disabled;
            ok = ModAddon::SetCtgpEnabled(package.root(), enable, &error);
        } else if (row - 1 < mod_entries.size()) {
            const auto& m = mod_entries[row - 1];
            ok = ModAddon::SetModEnabled(package.root(), m.name, !m.enabled, &error);
        }
        if (!ok) QMessageBox::warning(this, tr("Mods"), error);
        refreshMods();
        syncTextMerge();
    });
    connect(mod_remove_button, &QPushButton::clicked, this, [this] {
        const int row = mods_list->currentRow();
        const QString name = row <= 0 ? tr("CTGP DX") : (row - 1 < mod_entries.size() ? mod_entries[row - 1].name : QString{});
        if (name.isEmpty() || QMessageBox::question(this, tr("Quitar mod"), tr("¿Borrar %1 de la carpeta del juego?").arg(name)) != QMessageBox::Yes) return;
        QString error;
        const bool ok = row <= 0 ? ModAddon::RemoveCtgp(package.root(), &error) : ModAddon::RemoveMod(package.root(), name, &error);
        if (!ok) QMessageBox::warning(this, tr("Mods"), error);
        refreshMods();
        syncTextMerge();
    });
    connect(mod_recomp_button, &QPushButton::clicked, this, [this] {
        QMessageBox::information(this, tr("CTGP DX"), ModAddon::CodeRecompBlocker(mod_info));
    });
    connect(this, &MainWindow::importProgress, this, [this](int done, int total, const QString& name) {
        mod_progress->setMaximum(total);
        mod_progress->setValue(done);
        mod_progress->setFormat(QStringLiteral("%1 / %2  %3").arg(done).arg(total).arg(name));
    });
    connect(this, &MainWindow::importFinished, this, [this](bool ok, const QString& error) {
        importing = false;
        mod_progress->hide();
        if (!ok || !error.isEmpty()) QMessageBox::warning(this, tr("Importar"), error.isEmpty() ? tr("No se ha importado nada.") : error);
        refreshMods();
        syncTextMerge();
    });

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(mod_add_folder_button);
    buttons->addWidget(mod_add_zip_button);
    buttons->addWidget(mod_import_button);
    buttons->addStretch();
    auto* buttons2 = new QHBoxLayout;
    buttons2->addWidget(mod_toggle_button);
    buttons2->addWidget(mod_remove_button);
    buttons2->addStretch();
    buttons2->addWidget(mod_recomp_button);
    layout->addWidget(note);
    layout->addWidget(mods_list, 1);
    layout->addWidget(mod_details);
    layout->addWidget(mod_progress);
    layout->addLayout(buttons);
    layout->addLayout(buttons2);
    return page;
}

void MainWindow::refreshMods() {
    const QString root = package.root();
    const bool have_root = !root.isEmpty();
    const bool idle = !importing && game.state() == QProcess::NotRunning;
    const int keep_row = mods_list->currentRow();
    const QSignalBlocker block(mods_list);
    mods_list->clear();
    const auto ctgp = have_root ? ModAddon::CtgpState(root) : ModAddon::State::NotImported;
    const auto add_item = [this](const QString& text, bool checkable, bool checked) {
        auto* item = new QListWidgetItem(text);
        item->setFlags(item->flags() | (checkable ? Qt::ItemIsUserCheckable : Qt::NoItemFlags));
        if (checkable) item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
        mods_list->addItem(item);
    };
    add_item(ctgp == ModAddon::State::NotImported ? tr("CTGP DX  (no importado)")
             : ctgp == ModAddon::State::Enabled   ? tr("CTGP DX")
                                                  : tr("CTGP DX  (desactivado)"),
             ctgp != ModAddon::State::NotImported, ctgp == ModAddon::State::Enabled);
    mod_entries = have_root ? ModAddon::ListMods(root) : QList<ModAddon::ModEntry>{};
    for (const auto& m : mod_entries) {
        add_item(m.name + (m.has_exefs ? tr("  [tiene parches de código]") : QString{}), true, m.enabled);
    }
    mods_list->setCurrentRow(qBound(0, keep_row < 0 ? 0 : keep_row, mods_list->count() - 1));
    const int row = mods_list->currentRow();

    QString text;
    bool selected_enabled = true, selected_exists = true;
    if (row <= 0) {
        switch (ctgp) {
        case ModAddon::State::NotImported:
            text = tr("CTGP DX todavía no está añadido. Elige tu propio zip de CTGP Deluxe (la versión para Mario Kart 8 Deluxe, la que tiene una "
                      "carpeta atmosphere/contents) y sus 64 circuitos se añaden como copas extra con sus nombres, imágenes y música. No se cambia "
                      "ningún circuito del juego. Elige una copa en el menú del juego (F10, \"Copa extra\") antes de empezar un Gran Premio. El código "
                      "del pack no puede ejecutarse en esta versión recompilada; mira \"Sobre esta conversión\".");
            selected_exists = false;
            break;
        case ModAddon::State::Enabled:
        case ModAddon::State::Disabled: {
            selected_enabled = ctgp == ModAddon::State::Enabled;
            text = selected_enabled ? tr("CTGP DX: añadido y activado. 16 copas extra (menú F10, \"Copa extra\"); los circuitos del juego no se tocan.")
                                    : tr("CTGP DX: añadido, desactivado (el juego funciona normal).");
            text += QLatin1Char('\n') + ModAddon::CodeRecompBlocker(mod_info);
            break;
        }
        }
    } else if (row - 1 < mod_entries.size()) {
        const auto& m = mod_entries[row - 1];
        selected_enabled = m.enabled;
        text = tr("%1: %2, %3 archivos (%4 MB).").arg(m.name, m.enabled ? tr("activado") : tr("desactivado")).arg(m.files).arg(m.bytes / (1024 * 1024));
        text += QLatin1Char('\n') + (m.has_romfs ? tr("Datos (romfs): se aplican sobre los archivos del juego.") : tr("Sin datos romfs."));
        if (m.has_exefs) text += QLatin1Char('\n') + tr("Parches de código (exefs): esta versión recompilada no puede aplicarlos, así que se ignoran.");
    } else {
        selected_exists = false;
    }
    mod_details->setText(text);
    mod_import_button->setEnabled(have_root && idle);
    mod_add_folder_button->setEnabled(have_root && idle);
    mod_add_zip_button->setEnabled(have_root && idle);
    mod_toggle_button->setEnabled(idle && selected_exists);
    mod_toggle_button->setText(selected_enabled ? tr("Desactivar") : tr("Activar"));
    mod_remove_button->setEnabled(idle && selected_exists);
    mod_recomp_button->setEnabled(row <= 0 && ctgp != ModAddon::State::NotImported);
    mod_recomp_button->setToolTip(tr("Explica por qué el código del pack todavía no se puede recompilar en esta versión."));
}

void MainWindow::runImport(const std::function<bool(QString*, const std::function<bool(int, int, const QString&)>&)>& job) {
    importing = true;
    import_cancel = false;
    mod_progress->setValue(0);
    mod_progress->show();
    refreshMods();
    QThread* worker = QThread::create([this, job] {
        QString error;
        const bool ok = job(&error, [this](int done, int total, const QString& name) {
            emit importProgress(done, total, name);
            return !import_cancel.load();
        });
        emit importFinished(ok, error);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

namespace {

// Reads the game's own UI/<lang>/message.sarc files (no mods, update applied) into `dir`; the game exits after writing them.
bool DumpVanillaUi(const QString& exe, const QString& workDir, const QStringList& args, QProcessEnvironment env, const QString& dir, QString* error) {
    QDir(dir).removeRecursively();
    QProcess dump;
    env.insert(QStringLiteral("KARTRECOMP_ROMFS_DUMP"), QStringLiteral("tree=UI=%1/UI=message.sarc").arg(Portable::RelativeTo(workDir, dir)));
    env.insert(QStringLiteral("KARTRECOMP_NO_MODS"), QStringLiteral("1"));
    dump.setProcessEnvironment(env);
    dump.setWorkingDirectory(workDir);
    dump.start(exe, args);
    if (!dump.waitForFinished(240000) || dump.exitCode() != 0) {
        if (error) *error = QStringLiteral("No se pudieron leer los archivos de textos del juego (¿está el juego instalado?)");
        return false;
    }
    return true;
}

} // namespace

void MainWindow::syncTextMerge() {
    const QString root = package.root();
    if (importing || root.isEmpty()) return;
    if (!ModAddon::NeedsTextMerge(root)) {
        if (!ModAddon::HasTextConflict(root)) {
            QString error;
            ModAddon::UpdateTextMerge(root, {}, &error); // removes a stale merged copy when the conflict is gone
        }
        return;
    }
    const QStringList args = package.launchArguments(false);
    const auto env = package.launchEnvironment();
    const QString exe = package.executable(), workDir = package.workingDirectory();
    runImport([root, args, env, exe, workDir](QString* error, const std::function<bool(int, int, const QString&)>& progress) {
        progress(0, 1, QStringLiteral("combinando los textos de los mods"));
        const QString vanilla = ModAddon::VanillaUiDir(root);
        if (!QFileInfo::exists(QDir(vanilla).filePath(QStringLiteral("UI/USen/message.sarc"))) && !DumpVanillaUi(exe, workDir, args, env, vanilla, error)) return false;
        const bool ok = ModAddon::UpdateTextMerge(root, vanilla, error);
        progress(1, 1, QStringLiteral("hecho"));
        return ok;
    });
}

// CTGP DX's cups and other mods exclude each other. Returns false when the user keeps things as they were / cancels.
// `ctgp_wanted` = the user just ticked CTGP, `other_wanted` = another mod was just ticked; both false = the game is about to start.
bool MainWindow::resolveCtgpConflict(bool ctgp_wanted, bool other_wanted) {
    const QString root = package.root();
    if (ModAddon::CtgpState(root) == ModAddon::State::NotImported) return true;
    const bool ctgp_on = ctgp_wanted || ModAddon::CtgpState(root) == ModAddon::State::Enabled;
    QStringList others;
    for (const auto& m : ModAddon::ListMods(root)) {
        if (m.enabled) others << m.name;
    }
    if (!ctgp_on || (others.isEmpty() && !other_wanted)) return true;
    if (ctgp_wanted) {
        const auto answer = QMessageBox::question(this, tr("CTGP DX"),
            tr("Las copas de CTGP DX solo funcionan con todos los demás mods desactivados.\n\n¿Desactivar estos mods?\n\n%1").arg(others.join(QLatin1Char('\n'))));
        if (answer != QMessageBox::Yes) return false;
        for (const auto& name : others) ModAddon::SetModEnabled(root, name, false, nullptr);
        refreshMods();
        syncTextMerge();
        return true;
    }
    const auto answer = QMessageBox::question(this, tr("CTGP DX"),
        tr("Las copas de CTGP DX solo funcionan con todos los demás mods desactivados.\n\n¿Desactivar CTGP DX y mantener tus otros mods?"));
    if (answer != QMessageBox::Yes) return false;
    ModAddon::SetCtgpEnabled(root, false, nullptr);
    refreshMods();
    syncTextMerge();
    return true;
}

void MainWindow::importCtgp() {
    const QString zip = QFileDialog::getOpenFileName(this, tr("Elige el zip de CTGP DX"), {}, tr("Archivos zip (*.zip)"));
    Portable::EnterAppDir();
    if (zip.isEmpty()) return;
    const auto info = ModAddon::AnalyzeCtgpPack(zip);
    if (!info.valid) {
        QMessageBox::warning(this, tr("CTGP DX"), info.error);
        return;
    }
    mod_info = info;
    if (QMessageBox::question(this, tr("Añadir CTGP DX"),
                              tr("Esto añade los 64 circuitos de CTGP DX como copas extra. No se cambia ni se sustituye ningún circuito del juego.\n\n%1\n\n"
                                 "Tarda unos minutos y ocupa unos 5 GB. ¿Continuar?").arg(ModAddon::CodeRecompBlocker(info))) != QMessageBox::Yes) return;
    const QString root = package.root();
    const QString out = QDir(root).filePath(QStringLiteral("mods/%1/CTGP DX Cups").arg(QLatin1String(ModAddon::kMk8dTitleId)));
    // Where the converted mod goes; an older conversion (or a disabled one) is replaced.
    const QStringList args = package.launchArguments(false);
    const auto env = package.launchEnvironment();
    const QString exe = package.executable(), workDir = package.workingDirectory();
    runImport([zip, root, out, args, env, exe, workDir](QString* error, const std::function<bool(int, int, const QString&)>& progress) {
        // The game's own message files come from the game itself (it can list and extract its RomFS), so the names are edited in the user's copy.
        const QString tmp = ModAddon::VanillaUiDir(root);
        if (!DumpVanillaUi(exe, workDir, args, env, tmp, error)) return false;
        CtgpConvert::Options options;
        options.cups_list_path = QDir(root).filePath(QStringLiteral("extra_cups.txt"));
        QString old_error;
        ModAddon::SetModEnabled(root, QStringLiteral("CTGP DX (tracks)"), false, &old_error); // the old slot-replacing conversion must not stay on
        return CtgpConvert::Convert(zip, tmp, out, options, error, progress);
    });
}

void MainWindow::appendLog(const QString& text) {
    // suyu colours its console output; strip the ANSI escapes for the log view.
    static const QRegularExpression ansi(QStringLiteral("\x1b\\[[0-9;]*m"));
    QString clean = text;
    clean.remove(ansi);
    log_view->moveCursor(QTextCursor::End);
    log_view->insertPlainText(clean);
    log_view->moveCursor(QTextCursor::End);
}
