#include "EditorWindow.h"

#include <mutex>

#include "nmmLib/nmmlog.h"

#include "synthLib/midiBufferParser.h"

#if NMM_EMBEDDED_EDITOR
#include "MainComponent.h"
#include "midi/ConnectionManager.h"
#include "midi/MidiDeviceManager.h"
#include "ui/AppTheme.h"
#include "ui/ThemeRegistry.h"
#include "ui/EditorOptionsDialog.h"
#include "model/PchFileIO.h"
#endif

namespace nmm
{
	// the emulator as the editor's in-process synth
	class EditorWindow::Synth
#if NMM_EMBEDDED_EDITOR
		: public EmbeddedSynth
#endif
	{
	public:
		Synth(std::string _name, SendToSynth _send) : m_name(std::move(_name)), m_send(std::move(_send)) {}

		juce::String getName() const { return juce::String(m_name); }

		void sendToSynth(const std::vector<uint8_t>& _data)
		{
			if(_data.empty() || !m_send)
				return;
			synthLib::SMidiEvent ev(synthLib::MidiEventSource::Editor);
			if(_data[0] == 0xf0)
				ev.sysex.assign(_data.begin(), _data.end());
			else
				synthLib::setShortMessage(ev, _data.data(), _data.size());
			m_send(ev);
		}

		void setReceiver(std::function<void(const juce::MidiMessage&)> _receiver)
		{
			std::lock_guard lock(m_mutex);
			m_receiver = std::move(_receiver);
		}

		void onSynthMidiOut(const synthLib::SMidiEvent& _ev)
		{
			std::lock_guard lock(m_mutex);
			if(!m_receiver)
				return;
			if(!_ev.sysex.empty())
				m_receiver(juce::MidiMessage(_ev.sysex.data(), static_cast<int>(_ev.sysex.size())));
			else
			{
				const auto len = synthLib::MidiBufferParser::lengthFromStatusByte(_ev.a);
				if(len == 1) m_receiver(juce::MidiMessage(_ev.a));
				else if(len == 2) m_receiver(juce::MidiMessage(_ev.a, _ev.b));
				else m_receiver(juce::MidiMessage(_ev.a, _ev.b, _ev.c));
			}
		}

	private:
		std::string m_name;
		SendToSynth m_send;
		std::mutex m_mutex;
		std::function<void(const juce::MidiMessage&)> m_receiver;
	};

	class EditorWindow::Content : public juce::Component
	{
	public:
#if NMM_EMBEDDED_EDITOR
		explicit Content(juce::ApplicationProperties& _props)
		{
			m_main = std::make_unique<MainComponent>(_props);
			m_menu = std::make_unique<juce::MenuBarComponent>(m_main.get());
			addAndMakeVisible(*m_menu);
			addAndMakeVisible(*m_main);
			setSize(1280, 800);
		}
		~Content() override
		{
			m_menu.reset();
			m_main.reset();
		}
		void resized() override
		{
			auto b = getLocalBounds();
			m_menu->setBounds(b.removeFromTop(juce::LookAndFeel::getDefaultLookAndFeel().getDefaultMenuBarHeight()));
			m_main->setBounds(b);
		}
		MainComponent* main() const { return m_main.get(); }
	private:
		std::unique_ptr<MainComponent> m_main;
		std::unique_ptr<juce::MenuBarComponent> m_menu;
#else
		explicit Content(juce::ApplicationProperties&) { setSize(400, 100); }
#endif
	};

	bool EditorWindow::isAvailable()
	{
		return NMM_EMBEDDED_EDITOR != 0;
	}

	EditorWindow::EditorWindow(const std::string& _synthName, SendToSynth _sendToSynth, std::function<void()> _onClose)
		: DocumentWindow("Animatek NME - Nord Micro Modular emulator", juce::Colours::darkgrey, allButtons)
		, m_onClose(std::move(_onClose))
		, m_synth(std::make_unique<Synth>(_synthName, std::move(_sendToSynth)))
	{
#if NMM_EMBEDDED_EDITOR
		MidiDeviceManager::setEmbeddedSynth(m_synth.get());
#endif

		juce::PropertiesFile::Options options;
		options.applicationName = "NordMicroModularEmulator-NME";
		options.filenameSuffix = ".settings";
		options.osxLibrarySubFolder = "Application Support";
		m_properties.setStorageParameters(options);

		// point the editor at the embedded synth before it looks for its last connection
		if(auto* settings = m_properties.getUserSettings())
		{
#if NMM_EMBEDDED_EDITOR
			settings->setValue("midiInputDevice", juce::String(EmbeddedSynth::deviceId()));
			settings->setValue("midiOutputDevice", juce::String(EmbeddedSynth::deviceId()));
#endif
			settings->setValue("midiInputName", juce::String(_synthName));
			settings->setValue("midiOutputName", juce::String(_synthName));
			settings->saveIfNeeded();
		}

#if NMM_EMBEDDED_EDITOR
		const auto savedOptions = EditorOptions::load(m_properties.getUserSettings());
		AppTheme::setPalette(ThemeRegistry::get(savedOptions.uiThemeIndex).app);
#endif

		setUsingNativeTitleBar(true);
		m_content = std::make_unique<Content>(m_properties);
		setContentNonOwned(m_content.get(), true);
		setResizable(true, true);
		centreWithSize(getWidth(), getHeight());

		if(auto* settings = m_properties.getUserSettings())
		{
			const auto state = settings->getValue("mainWindowState");
			if(state.isNotEmpty())
				restoreWindowStateFromString(state);
		}

		setVisible(true);
		NMMLOG("editor window opened for synth '%s'", _synthName.c_str());
	}

	EditorWindow::~EditorWindow()
	{
		if(auto* settings = m_properties.getUserSettings())
		{
			settings->setValue("mainWindowState", getWindowStateAsString());
			settings->saveIfNeeded();
		}
		m_content.reset();
#if NMM_EMBEDDED_EDITOR
		MidiDeviceManager::setEmbeddedSynth(nullptr);
#endif
		m_synth.reset();
	}

	void EditorWindow::onSynthMidiOut(const synthLib::SMidiEvent& _ev)
	{
		if(m_synth)
			m_synth->onSynthMidiOut(_ev);
	}

	void EditorWindow::closeButtonPressed()
	{
		if(m_onClose)
			m_onClose();
	}

	bool EditorWindow::isConnected() const
	{
#if NMM_EMBEDDED_EDITOR
		return m_content && m_content->main() && m_content->main()->getConnectionManager().isConnected();
#else
		return false;
#endif
	}

	std::string EditorWindow::getCurrentPatchText(std::string& _name) const
	{
#if NMM_EMBEDDED_EDITOR
		if(!m_content || !m_content->main())
			return {};
		auto* main = m_content->main();
		const auto* patch = main->getSlotPatch(0);
		if(!patch)
			return {};
		_name = patch->getName().toStdString();
		PchFileIO io(main->getModuleDescriptions());
		return io.toText(*patch).toStdString();
#else
		(void)_name;
		return {};
#endif
	}

	std::string EditorWindow::getStatus() const
	{
#if NMM_EMBEDDED_EDITOR
		if(!m_content || !m_content->main())
			return {};
		return m_content->main()->getConnectionManager().getStatus().message.toStdString();
#else
		return "editor not built in";
#endif
	}
}
