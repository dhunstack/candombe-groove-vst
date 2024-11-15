#pragma once

#include "shared_plugin_helpers/shared_plugin_helpers.h"
//#include <vector>
#include <torch/torch.h>
#include "../Includes/Configs_Model.h"
#include "../../PluginCode/deploy.h"
#include "../Includes/APVTSMediatorThread.h"
#include "../Includes/LockFreeQueue.h"
#include "../Includes/GenerationEvent.h"
#include "../Includes/APVTSMediatorThread.h"
#include <mutex>

// #include "gui/CustomGuiTextEditors.h"

using namespace std;

struct GenerationsToDisplay {
private:
    bool policy_accessed_already{false};
    double fs {44100};
    double qpm {-1};
    double playhead_pos {0};
    bool empty_sequence_received{false};

public:
    PlaybackPolicies policy;
    juce::MidiMessageSequence sequence_to_display;
    std::mutex mutex;


    void setSequence(const juce::MidiMessageSequence& sequence) {
        std::lock_guard<std::mutex> lock(mutex);
        sequence_to_display = sequence;
        if (sequence_to_display.getNumEvents() == 0) {
            empty_sequence_received = true;
        }
    }

    void setFs(double fs_) {
        std::lock_guard<std::mutex> lock(mutex);
        fs = fs_;
    }

    void setQpm(double qpm_) {
        std::lock_guard<std::mutex> lock(mutex);
        qpm = qpm_;
    }

    void setPlayheadPos(double playhead_pos_) {
        std::lock_guard<std::mutex> lock(mutex);
        playhead_pos = playhead_pos_;
    }

    void setPolicy(PlaybackPolicies policy_) {
        std::lock_guard<std::mutex> lock(mutex);
        policy_accessed_already = false;
        policy = policy_;
    }

    std::optional<juce::MidiMessageSequence> getSequence() {
        std::lock_guard<std::mutex> lock(mutex);
        juce::MidiMessageSequence sequence_to_display_copy{sequence_to_display};
        sequence_to_display.clear();
        if (sequence_to_display_copy.getNumEvents() > 0 || empty_sequence_received) {
            empty_sequence_received = false;
            return sequence_to_display_copy;
        } else {
            return std::nullopt;
        }
    }

    std::optional<PlaybackPolicies> getPolicy() {
        std::lock_guard<std::mutex> lock(mutex);
        if (policy_accessed_already) {
            return std::nullopt;
        } else {
            policy_accessed_already = true;
            return policy;
        }
    }

    std::optional<double> getFs() {
        std::lock_guard<std::mutex> lock(mutex);
        return fs;
    }

    std::optional<double> getQpm() {
        std::lock_guard<std::mutex> lock(mutex);
        return qpm;
    }

    std::optional<double> getPlayheadPos() {
        std::lock_guard<std::mutex> lock(mutex);
        return playhead_pos;
    }

};

struct StandAloneParams {
    float qpm{-1};
    int is_playing{0};
    int is_recording{0};
    int denominator{4};
    int numerator{4};

    int64_t TimeInSamples{0};
    double TimeInSeconds{0};
    double PpqPosition{0};

    StaticLockFreeQueue<vector<float>, queue_settings::APVM_que_size>* APVTM2NMP_StandaloneParameters_Que_ptr{};

    explicit StandAloneParams(juce::AudioProcessorValueTreeState* apvtsPntr,
                              StaticLockFreeQueue<vector<float>, queue_settings::APVM_que_size>* APVTM2NMP_StandaloneParameters_Que_ptr_) {
        APVTM2NMP_StandaloneParameters_Que_ptr = APVTM2NMP_StandaloneParameters_Que_ptr_;
        // initialize
        qpm = *apvtsPntr->getRawParameterValue(label2ParamID("TempoStandalone"));
        is_playing = int(*apvtsPntr->getRawParameterValue(label2ParamID("IsPlayingStandalone")));
        is_recording = int(*apvtsPntr->getRawParameterValue(label2ParamID("IsRecordingStandalone")));
        denominator = int(*apvtsPntr->getRawParameterValue(label2ParamID("TimeSigDenominatorStandalone")));
        numerator = int(*apvtsPntr->getRawParameterValue(label2ParamID("TimeSigNumeratorStandalone")));

    }

    // call update at the beginning of each processBlock
    bool update() {
        bool changed = false;

        if (APVTM2NMP_StandaloneParameters_Que_ptr->getNumReady() > 0) {
            vector<float> new_params = APVTM2NMP_StandaloneParameters_Que_ptr->getLatestOnly();
            qpm = new_params[0];
            is_playing = int(new_params[1]);
            is_recording = int(new_params[2]);
            denominator = int(new_params[3]);
            numerator = int(new_params[4]);
            changed = true;
        }

        return changed;
    }

    // call this after setting the PositionInfo data
    void PreparePlayheadForNextFrame(int64_t buffer_size, double fs) {
        if (is_playing) {
            TimeInSamples += (buffer_size);
            TimeInSeconds += ((double) buffer_size / fs);
            PpqPosition += ((double) buffer_size / fs * qpm / 60);
        } else {
            TimeInSamples = 0;
            TimeInSeconds = 0;
            PpqPosition = 0;
        }
    }

};

class NeuralMidiFXPluginProcessor : public PluginHelpers::ProcessorBase {

public:

    NeuralMidiFXPluginProcessor();

    ~NeuralMidiFXPluginProcessor() override;

    void processBlock(juce::AudioBuffer<float> &buffer, juce::MidiBuffer &midiMessages) override;

    juce::AudioProcessorEditor* createEditor() override;

    // Queues
    unique_ptr<StaticLockFreeQueue<EventFromHost, queue_settings::NMP2DPL_que_size>> NMP2DPL_Event_Que;
    unique_ptr<StaticLockFreeQueue<GenerationEvent, queue_settings::DPL2NMP_que_size>> DPL2NMP_GenerationEvent_Que;
    unique_ptr<StaticLockFreeQueue<juce::MidiMessageSequence, 32>> NMP2GUI_IncomingMessageSequence;

    // APVTS Queues
    unique_ptr<StaticLockFreeQueue<GuiParams, queue_settings::APVM_que_size>> APVM2DPL_GuiParams_Que;
    unique_ptr<StaticLockFreeQueue<vector<float>, queue_settings::APVM_que_size>>
        APVTM2NMP_StandaloneParameters_Que;

    // Drag/Drop Midi Queues
    unique_ptr<StaticLockFreeQueue<juce::MidiFile, 4>> GUI2DPL_DroppedMidiFile_Que;
    unique_ptr<StaticLockFreeQueue<juce::MidiFile, 4>> DPL2GUI_GenerationMidiFile_Que;

    // Threads used for generating patterns in the background
    shared_ptr<PluginDeploymentThread> deploymentThread;


    // APVTS
    juce::AudioProcessorValueTreeState apvts;
    unique_ptr<APVTSMediatorThread> apvtsMediatorThread;

    void getStateInformation(juce::MemoryBlock &destData) override;
    void setStateInformation(const void *data, int sizeInBytes) override;

    // realtime playback info
    unique_ptr<RealTimePlaybackInfo> realtimePlaybackInfo{};

    // Playback Data
    PlaybackPolicies playbackPolicies{};
    juce::MidiMessageSequence playbackMessageSequence{};
    time_ time_anchor_for_playback{};

    // mutex protected structures for interacting with the GUI
    GenerationsToDisplay generationsToDisplay{};
    mutex playbackAnchorMutex;
    time_ TimeAnchor;
    bool shouldSendTimeAnchorToGUI{false};

    // standalone
    unique_ptr<StandAloneParams> standAloneParams;

    // CrossThreadPianoRollData
    unique_ptr<MidiVisualizersData> midiVisualizersData {};

    // CrossThreadAudioVisualizerData
    unique_ptr<AudioVisualizersData> audioVisualizersData {};

private:
    // =========  Queues for communicating Between the main threads in processor  ===============

    // holds the playhead position for displaying on GUI
    time_ playhead_start_time{};
    std::optional<juce::MidiMessage> getMessageIfToBePlayed(
            time_ now_, const juce::MidiMessage &msg,
            int buffSize, double fs, double qpm) const;

    //  midiBuffer to fill up with generated data
    juce::MidiBuffer tempBuffer;

    // Parameter Layout for apvts
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // EventFromHost Placeholders for cross buffer events
    EventFromHost last_frame_meta_data{};
    std::optional<EventFromHost> NewBarEvent;
    std::optional<EventFromHost> NewTimeShiftEvent;
    juce::MidiMessageSequence incoming_messages_sequence;

    // Gets DAW info and midi messages,
    // Wraps messages as Events
    // then sends them to the DeploymentThread via the NMP2DPL_Event_Que
    void sendReceivedInputsAsEvents(
            MidiBuffer &midiMessages, const Optional<AudioPlayHead::PositionInfo> &Pinfo,
            double fs,
            int buffSize);


    // utility methods
    static void PrintMessage(const std::string& input);

    // MidiIO Standalone
    unique_ptr<MidiOutput> mVirtualMidiOutput;

    bool shouldActStandalone{false};
    AudioPlayHead::TimeSignature timeSig;

    EventFromHost eventFromHost{};
    BufferMetaData bufferMetaData{};

    bool should_send_audio {needs_audio};
    juce::AudioBuffer<float> audio_buffer{};
};
