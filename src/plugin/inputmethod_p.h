/*
 * Copyright (C) 2013-2016 Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "inputmethod.h"
#include "coreutils.h"

#include "device.h"
#include "editor.h"
#include "feedback.h"
#include "gettext.h"
#include "greeterstatus.h"
#include "iconprovider.h"
#include "keyboardgeometry.h"
#include "keyboardsettings.h"
#include "theme.h"

#include "models/wordribbon.h"
#include "logic/eventhandler.h"
#include "logic/wordengine.h"

#include <maliit/plugins/abstractinputmethodhost.h>
#include <maliit/plugins/abstractpluginsetting.h>

#include <QtQuick>
#include <QStringList>
#include <qglobal.h>
#include <QDebug>

#include <memory>

namespace
{
// Qt::WindowType enum has no option for an Input Method window type. This is a magic value
// used by ubuntumirclient QPA for special clients to request input method windows from Mir.
const Qt::WindowType InputMethodWindowType = (Qt::WindowType)(0x00000080 | Qt::WindowType::Window);
}

using namespace MaliitKeyboard;

typedef QScopedPointer<Maliit::Plugins::AbstractPluginSetting> ScopedSetting;
typedef QSharedPointer<MKeyOverride> SharedOverride;
typedef QMap<QString, SharedOverride>::const_iterator OverridesIterator;

QQuickView *createWindow(MAbstractInputMethodHost *host)
{
    QScopedPointer<QQuickView> view(new QQuickView);

    QSurfaceFormat format;
    format.setAlphaBufferSize(8);
    view->setFormat(format);
    view->setColor(QColor(Qt::transparent));

    host->registerWindow(view.data(), Maliit::PositionCenterBottom);

    return view.take();
}

class InputMethodPrivate
{
public:
    InputMethod* q;
    Editor editor;
    SharedOverride actionKeyOverrider;
    Logic::EventHandler event_handler;
    MAbstractInputMethodHost* host;
    QQuickView* view;

    bool autocapsEnabled;
    bool wordEngineEnabled;
    InputMethod::TextContentType contentType;
    QString activeLanguage;
    QStringList enabledLanguages;
    Qt::ScreenOrientation appsCurrentOrientation;
    QString keyboardState;
    bool hasSelection;

    QString preedit;

    KeyboardGeometry *m_geometry;
    KeyboardSettings m_settings;
    GreeterStatus *m_greeterStatus;

    std::unique_ptr<Feedback> m_feedback;
    std::unique_ptr<Theme> m_theme;
    std::unique_ptr<Device> m_device;
    std::unique_ptr<IconProvider> m_iconProvider;
    std::unique_ptr<Gettext> m_gettext;

    WordRibbon* wordRibbon;

    int previous_position;

    QStringList languagesPaths;
    QString currentPluginPath;

    bool animationEnabled = true;

    explicit InputMethodPrivate(InputMethod * const _q,
                                MAbstractInputMethodHost *host)
        : q(_q)
        , editor(EditorOptions(), new Model::Text, new Logic::WordEngine)
        , event_handler()
        , host(host)
        , view(0)
        , autocapsEnabled(false)
        , wordEngineEnabled(false)
        , contentType(InputMethod::FreeTextContentType)
        , activeLanguage(QStringLiteral("en"))
        , enabledLanguages(activeLanguage)
        , appsCurrentOrientation(qGuiApp->primaryScreen()->orientation())
        , keyboardState(QStringLiteral("CHARACTERS"))
        , hasSelection(false)
        , preedit()
        , m_geometry(new KeyboardGeometry(q))
        , m_settings()
        , m_greeterStatus(new GreeterStatus())
        , m_feedback(std::make_unique<Feedback>(&m_settings))
        , m_theme(std::make_unique<Theme>(&m_settings))
        , m_device(std::make_unique<Device>(&m_settings))
        , m_iconProvider(std::make_unique<IconProvider>(m_theme.get()))
        , m_gettext(std::make_unique<Gettext>())
        , wordRibbon(new WordRibbon)
        , previous_position(-1)
    {
        view = createWindow(host);

        m_device->setWindow(view);

        editor.setHost(host);

        //! connect wordRibbon
        QObject::connect(&event_handler, &MaliitKeyboard::Logic::EventHandler::wordCandidatePressed,
                         wordRibbon, &MaliitKeyboard::WordRibbon::onWordCandidatePressed);

        QObject::connect(&event_handler, &MaliitKeyboard::Logic::EventHandler::wordCandidateReleased,
                         wordRibbon, &MaliitKeyboard::WordRibbon::onWordCandidateReleased);

        QObject::connect(&editor,  &MaliitKeyboard::AbstractTextEditor::wordCandidatesChanged,
                         wordRibbon, &MaliitKeyboard::WordRibbon::onWordCandidatesChanged);

        QObject::connect(wordRibbon, &MaliitKeyboard::WordRibbon::wordCandidateSelected,
                         &editor,  &MaliitKeyboard::AbstractTextEditor::onWordCandidateSelected);

        QObject::connect(wordRibbon, &MaliitKeyboard::WordRibbon::userCandidateSelected,
                         &editor,  &MaliitKeyboard::AbstractTextEditor::addToUserDictionary);

        QObject::connect(&editor,  &MaliitKeyboard::AbstractTextEditor::preeditEnabledChanged,
                         wordRibbon, &MaliitKeyboard::WordRibbon::setWordRibbonVisible);

        QObject::connect(wordRibbon, &MaliitKeyboard::WordRibbon::wordCandidateSelected,
                         editor.wordEngine(), &MaliitKeyboard::Logic::AbstractWordEngine::onWordCandidateSelected);

        QObject::connect(editor.wordEngine(), &MaliitKeyboard::Logic::AbstractWordEngine::commitTextRequested,
                         &editor, &MaliitKeyboard::AbstractTextEditor::replaceAndCommitPreedit);


    #ifdef DISABLED_FLAGS_FROM_SURFACE
        view->setFlags(Qt::Dialog | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                          | Qt::X11BypassWindowManagerHint | Qt::WindowDoesNotAcceptFocus);
    #endif
        view->setWindowState(Qt::WindowNoState);

        QSurfaceFormat format = view->format();
        format.setAlphaBufferSize(8);
        view->setFormat(format);
        view->setColor(QColor(Qt::transparent));

        updateLanguagesPaths();

        // TODO: Figure out whether two views can share one engine.
        QQmlEngine *const engine(view->engine());

        QString prefix = qgetenv("KEYBOARD_PREFIX_PATH");
        if (!prefix.isEmpty()) {
            engine->addImportPath(prefix + QDir::separator() + MALIIT_KEYBOARD_QML_DIR);
            engine->addImportPath(prefix + QDir::separator() + QStringLiteral(MALIIT_KEYBOARD_QML_DIR) + QDir::separator() + "keys");
        } else {
            engine->addImportPath(MALIIT_KEYBOARD_QML_DIR);
            engine->addImportPath(QStringLiteral(MALIIT_KEYBOARD_QML_DIR) + QDir::separator() + "keys");
        }

        setContextProperties(engine->rootContext());

        // Add our image provider for handling icon themes
        engine->addImageProvider(QLatin1String("icon"), m_iconProvider.get());

        // workaround: resizeMode not working in current qpa imlementation
        // http://qt-project.org/doc/qt-5.0/qtquick/qquickview.html#ResizeMode-enum
        view->setResizeMode(QQuickView::SizeRootObjectToView);

        if (QGuiApplication::platformName() == QLatin1String("ubuntumirclient")) {
            view->setFlags(InputMethodWindowType); /* Mir-only OSK window type */
        }

        // When keyboard geometry changes, update the window's input mask
        QObject::connect(m_geometry, &KeyboardGeometry::visibleRectChanged, view, [this]() {
            view->setMask(m_geometry->visibleRect().toRect());
        });
    }

    void setLayoutOrientation(Qt::ScreenOrientation screenOrientation)
    {
        m_geometry->setOrientation(screenOrientation);
    }

    void setContextProperties(QQmlContext *qml_context)
    {
        qmlRegisterSingletonInstance("MaliitKeyboard", 2, 0, "Keyboard", q);
        qmlRegisterSingletonInstance("MaliitKeyboard", 2, 0, "Feedback", m_feedback.get());
        qmlRegisterSingletonInstance("MaliitKeyboard", 2, 0, "Theme", m_theme.get());
        qmlRegisterSingletonInstance("MaliitKeyboard", 2, 0, "Device", m_device.get());
        qmlRegisterSingletonInstance("MaliitKeyboard", 2, 0, "Gettext", m_gettext.get());
        qml_context->setContextProperty(QStringLiteral("maliit_input_method"), q);
        qml_context->setContextProperty(QStringLiteral("maliit_geometry"), m_geometry);
        qml_context->setContextProperty(QStringLiteral("maliit_event_handler"), &event_handler);
        qml_context->setContextProperty(QStringLiteral("maliit_wordribbon"), wordRibbon);
        qml_context->setContextProperty(QStringLiteral("maliit_word_engine"), editor.wordEngine());
        qml_context->setContextProperty(QStringLiteral("greeter_status"), m_greeterStatus);
    }

    void updateLanguagesPaths()
    {
        languagesPaths.clear();
        QString prefix = qgetenv("MALIIT_KEYBOARD_LANGUAGES_PATH");
        if (!prefix.isEmpty()) {
            languagesPaths.append(prefix);
        }
        languagesPaths.append(QStringLiteral(MALIIT_KEYBOARD_LANGUAGES_DIR));
        languagesPaths.append(m_settings.pluginPaths());
    }

    /*
     * register settings
     */
    void registerAudioFeedbackSoundSetting()
    {
        QObject::connect(&m_settings, &MaliitKeyboard::KeyboardSettings::keyPressAudioFeedbackSoundChanged,
                         q, &InputMethod::audioFeedbackSoundChanged);
    }

    void registerAudioFeedbackSetting()
    {
        QObject::connect(&m_settings, &MaliitKeyboard::KeyboardSettings::keyPressAudioFeedbackChanged,
                         q, &InputMethod::useAudioFeedbackChanged);
    }

    void registerHapticFeedbackSetting()
    {
        QObject::connect(&m_settings, &MaliitKeyboard::KeyboardSettings::keyPressHapticFeedbackChanged,
                         q, &InputMethod::useHapticFeedbackChanged);
    }

    void registerEnableMagnifier()
    {
        QObject::connect(&m_settings, SIGNAL(enableMagnifierChanged(bool)),
                         q, SIGNAL(enableMagnifierChanged()));
    }

    void registerAutoCorrectSetting()
    {
        QObject::connect(&m_settings, &MaliitKeyboard::KeyboardSettings::autoCompletionChanged,
                         q, &InputMethod::onAutoCorrectSettingChanged);
        editor.setAutoCorrectEnabled(m_settings.autoCompletion());
    }

    void registerAutoCapsSetting()
    {
        QObject::connect(&m_settings, &MaliitKeyboard::KeyboardSettings::autoCapitalizationChanged,
                         q, &InputMethod::updateAutoCaps);
    }

    void registerWordEngineSetting()
    {
        QObject::connect(&m_settings, &MaliitKeyboard::KeyboardSettings::predictiveTextChanged,
                         editor.wordEngine(), &MaliitKeyboard::Logic::AbstractWordEngine::setWordPredictionEnabled);
        editor.wordEngine()->setWordPredictionEnabled(m_settings.predictiveText());

        QObject::connect(&m_settings, &MaliitKeyboard::KeyboardSettings::spellCheckingChanged,
                         editor.wordEngine(), &MaliitKeyboard::Logic::AbstractWordEngine::setSpellcheckerEnabled);
        editor.wordEngine()->setSpellcheckerEnabled(m_settings.spellchecking());
    }

    void registerActiveLanguage()
    {
        QObject::connect(&m_settings, &MaliitKeyboard::KeyboardSettings::activeLanguageChanged,
                         q, &InputMethod::setActiveLanguage);

        activeLanguage = m_settings.activeLanguage();
        qDebug() << "inputmethod_p.h registerActiveLanguage(): activeLanguage is:" << activeLanguage;
        q->setActiveLanguage(activeLanguage);
    }

    void registerEnabledLanguages()
    {
        QObject::connect(&m_settings, &MaliitKeyboard::KeyboardSettings::enabledLanguagesChanged,
                         q, &InputMethod::onEnabledLanguageSettingsChanged);
        q->onEnabledLanguageSettingsChanged();

        //registerSystemLanguage();
        //q->setActiveLanguage(activeLanguage);
    }

    void registerDoubleSpaceFullStop()
    {
        QObject::connect(&m_settings, &MaliitKeyboard::KeyboardSettings::doubleSpaceFullStopChanged,
                         q, &InputMethod::onDoubleSpaceSettingChanged);
        editor.setDoubleSpaceFullStopEnabled(m_settings.doubleSpaceFullStop());
    }

    void registerStayHidden()
    {
        QObject::connect(&m_settings, &MaliitKeyboard::KeyboardSettings::stayHiddenChanged,
                         q, &InputMethod::hide);
    }

    void registerPluginPaths()
    {
        QObject::connect(&m_settings, &MaliitKeyboard::KeyboardSettings::pluginPathsChanged,
                        q, &InputMethod::onPluginPathsChanged);
    }

    void registerOpacity()
    {
        QObject::connect(&m_settings, &MaliitKeyboard::KeyboardSettings::opacityChanged,
                        q, &InputMethod::opacityChanged);
    }

    void registerTheme()
    {
        QObject::connect(&m_settings, SIGNAL(themeChanged(QString)),
                        q, SIGNAL(themeChanged(QString)));
    }

    void closeOskWindow()
    {
        if (!view->isVisible())
            return;

        host->notifyImInitiatedHiding();

        m_geometry->setShown(false);

        editor.clearPreedit();

        view->setVisible(false);
    }
};
