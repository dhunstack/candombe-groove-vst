//
// Created by u153171 on 11/22/2023.
//

#include "DeploymentThread.h"

DeploymentThread::DeploymentThread(): juce::Thread("BackgroundDPLThread") {
    CustomPresetData = make_unique<CustomPresetDataDictionary>();
}

void DeploymentThread::startThreadUsingProvidedResources(
    StaticLockFreeQueue<EventFromHost, queue_settings::NMP2DPL_que_size> *NMP2DPL_Event_Que_ptr_,
    StaticLockFreeQueue<GuiParams, queue_settings::APVM_que_size> *APVM2NMD_Parameters_Que_ptr_,
    StaticLockFreeQueue<GenerationEvent, queue_settings::DPL2NMP_que_size> *DPL2NMP_GenerationEvent_Que_ptr_,
    StaticLockFreeQueue<juce::MidiFile, 4>* GUI2DPL_DroppedMidiFile_Que_ptr_,
    RealTimePlaybackInfo *realtimePlaybackInfo_ptr_,
    MidiVisualizersData* visualizerData_ptr_,
    AudioVisualizersData* audioVisualizersData_ptr_)
{


    NMP2DPL_Event_Que_ptr = NMP2DPL_Event_Que_ptr_;
    APVM2DPL_Parameters_Que_ptr = APVM2NMD_Parameters_Que_ptr_;
    DPL2NMP_GenerationEvent_Que_ptr = DPL2NMP_GenerationEvent_Que_ptr_;
    GUI2DPL_DroppedMidiFile_Que_ptr = GUI2DPL_DroppedMidiFile_Que_ptr_;
    realtimePlaybackInfo = realtimePlaybackInfo_ptr_;
    midiVisualizersData = visualizerData_ptr_;
    audioVisualizersData = audioVisualizersData_ptr_;

    // Start the thread. This function internally calls run() method. DO NOT CALL run() DIRECTLY.
    // ---------------------------------------------------------------------------------------------
    startThread();
}

void DeploymentThread::run() {

    // convert showMessage to a lambda function
    auto showMessage = [](const std::string& input) {
        // if input is multiline, split it into lines && print each line separately
        std::stringstream ss(input);
        std::string line;

        while (std::getline(ss, line)) {
            std::cout << clr::green << "[DPL] " << line << std::endl;
        }
    };

    // notify if the thread is still running
    bool bExit = threadShouldExit();

    double events_received_count = 0;

    std::optional<EventFromHost> new_event_from_DAW {};
    std::optional<MidiFileEvent> new_midi_event_dropped_manually {};
    bool shouldSendNewPlaybackPolicy;
    bool shouldSendNewPlaybackSequence;
    bool midiFileDroppedOnVisualizer;
    int cyclesToIgnoreTriggerButtons = -1; // when a preset is loaded we disable the trigger buttons for two iterations two ensure the preset doesn't trigger these buttons
    int cnt{0};

    cout << "Deployment Thread is running..." << endl;
    while (!bExit) {

        if (readyToStop) { break; } // check if thread is ready to be stopped
        if (APVM2DPL_Parameters_Que_ptr->getNumReady() > 0) {
            // print updated values for debugging
            gui_params = APVM2DPL_Parameters_Que_ptr
                             ->pop(); // pop the latest parameters from the queue

            cnt++;
            if (debugging_settings::DeploymentThread::print_received_gui_params) { // if set in Debugging.h
                showMessage(gui_params.getDescriptionOfUpdatedParams());
            }

        } else {
            gui_params.setChanged(false); // no change in parameters since last check
        }

        if (NMP2DPL_Event_Que_ptr->getNumReady() > 0) {
            new_event_from_DAW = NMP2DPL_Event_Que_ptr->pop();      // get the next available event

            events_received_count++;

            if (debugging_settings::DeploymentThread::print_input_events) { // if set in Debugging.h
                DisplayEvent(*new_event_from_DAW, false, events_received_count);   // display the event
            }

        } else {
            new_event_from_DAW = std::nullopt;
        }

        // check if a new midi file dropped on the visualizer
        midiFileDroppedOnVisualizer = false;
        if (midiVisualizersData != nullptr) {
            auto changed_visualizers =
                midiVisualizersData->get_visualizer_ids_with_user_dropped_new_sequences();
            if (!changed_visualizers.empty()) {
                midiFileDroppedOnVisualizer = true;
            }
            /*
            for (auto& [key, value] : *midiVisualizersData) {
                if (value.userDroppedNewSequence()) {
                    midiFileDroppedOnVisualizer = true;
                    break;
                }
            }*/
        }

        // check if a new audio file dropped on the visualizer
        bool audioFileDroppedOnVisualizer = false;
        if (audioVisualizersData != nullptr) {
            auto changed_visualizers =
                audioVisualizersData->get_visualizer_ids_with_user_dropped_new_audio();
            if (!changed_visualizers.empty()) {
                audioFileDroppedOnVisualizer = true;
            }

        }

        // scope lock mutex deploymentThread->preset_loaded_mutex
        // try to lock mutex, if not possible, skip the rest of the loop
        bool newPresAvail = CustomPresetData->hasTensorDataChanged();

        if (new_event_from_DAW.has_value() || gui_params.changed() || newPresAvail || midiFileDroppedOnVisualizer || audioFileDroppedOnVisualizer) {
            new_midi_event_dropped_manually = std::nullopt;

            if (newPresAvail) {
                cyclesToIgnoreTriggerButtons = 2;
            }
            if (cyclesToIgnoreTriggerButtons >= 0) {
                cyclesToIgnoreTriggerButtons--;
                gui_params.ignoreTriggerButtons();
            }


            auto status = deploy(
                new_midi_event_dropped_manually, new_event_from_DAW,
                gui_params.changed(), newPresAvail,
                midiFileDroppedOnVisualizer,
                audioFileDroppedOnVisualizer);
            gui_params.setChanged(false);

            shouldSendNewPlaybackPolicy = status.first;
            shouldSendNewPlaybackSequence = status.second;
            // push to next thread if a new input is provided
            if (shouldSendNewPlaybackPolicy) {
                // send to the main thread (NMP)
                if (playbackPolicy.IsReadyForTransmission()) {
                    DPL2NMP_GenerationEvent_Que_ptr->push(GenerationEvent(playbackPolicy));
                }
            }

            if (shouldSendNewPlaybackSequence) {
                // send to the main thread (NMP)
                DPL2NMP_GenerationEvent_Que_ptr->push(GenerationEvent(playbackSequence));
                cnt++;
            }

        }

        if (newPresAvail) {
            // notify that the preset has been loaded
            CustomPresetData->resetChangeFlags();
        }

        // check if notes received from a manually dropped midi file
        if (GUI2DPL_DroppedMidiFile_Que_ptr->getNumReady() > 0){
            auto midifile = GUI2DPL_DroppedMidiFile_Que_ptr->getLatestOnly();

            if (midifile.getNumTracks() > 0) {
                auto track = midifile.getTrack(0);
                for (int i = 0; i < track->getNumEvents(); ++i)
                {
                    auto msg_ = track->getEventPointer(i)->message;

                    if (debugging_settings::DeploymentThread::print_manually_dropped_midi_messages) { // if set in Debugging.h
                        showMessage("Midi Message Received from Dropped Midi File: ");
                        showMessage(msg_.getDescription().toStdString());
                    }

                    auto isFirst = (i == 0);
                    auto isLast = (i == track->getNumEvents() - 1);
                    new_midi_event_dropped_manually = MidiFileEvent(msg_, isFirst, isLast);
                    new_event_from_DAW = std::nullopt;
                    auto status =  deploy(new_midi_event_dropped_manually, new_event_from_DAW, false, false, false, false);
                    shouldSendNewPlaybackPolicy = status.first;
                    shouldSendNewPlaybackSequence = status.second;

                    // push to next thread if a new input is provided
                    if (shouldSendNewPlaybackPolicy) {
                        // send to the main thread (NMP)
                        if (playbackPolicy.IsReadyForTransmission()) {
                            DPL2NMP_GenerationEvent_Que_ptr->push(GenerationEvent(playbackPolicy));
                        }
                    }

                    if (shouldSendNewPlaybackSequence) {
                        // send to the main thread (NMP)
                        DPL2NMP_GenerationEvent_Que_ptr->push(GenerationEvent(playbackSequence));
                        cnt++;
                    }

                }
            }
        }



        // update event trackers accordingly if applicable
        if (new_event_from_DAW.has_value()) {

            if (new_event_from_DAW->isFirstBufferEvent()) { first_frame_metadata_event = *new_event_from_DAW; }
            else if (new_event_from_DAW->isNewBufferEvent()) { frame_metadata_event = *new_event_from_DAW; }
            else if (new_event_from_DAW->isNewBarEvent()) { last_bar_event = *new_event_from_DAW; }
            else if (new_event_from_DAW->isNewTimeShiftEvent()) { last_complete_note_duration_event = *new_event_from_DAW; }

            last_event = *new_event_from_DAW;
        }

        // check if thread is still running
        bExit = threadShouldExit();

        if (!new_event_from_DAW.has_value() && !gui_params.changed()) {
            // wait for a few ms to avoid burning the CPU if new data is not available
            sleep((int)thread_configurations::SingleMidiThread::waitTimeBtnIters);
        }
    }

}

void DeploymentThread::prepareToStop()
{
    // Need to wait enough to ensure the run() method is over before killing thread
    if (thread_configurations::SingleMidiThread::waitTimeBtnIters > 0) {
        this->stopThread(1000 * thread_configurations::SingleMidiThread::waitTimeBtnIters);
    } else {
        this->stopThread(1000);
    }

    readyToStop = true;
}

DeploymentThread::~DeploymentThread()
{
    if (!readyToStop) {
        prepareToStop();
    }
}

void DeploymentThread::DisplayEvent(const EventFromHost& event,
                                    bool compact_mode,
                                    double /*event_count*/)
{
    auto showMessage = [](const std::string& input) {
        // if input is multiline, split it into lines && print each line separately
        std::stringstream ss(input);
        std::string line;

        while (std::getline(ss, line)) {
            std::cout << clr::on_red << "[DPL] " << line << std::endl;
        }
    };

    if (event.isFirstBufferEvent() || !compact_mode) {
        auto dscrptn = event.getDescription().str();
        if (dscrptn.length() > 0) { showMessage(dscrptn); }
    } else {
        auto dscrptn = event.getDescriptionOfChangedFeatures(event, true).str();
        if (dscrptn.length() > 0) { showMessage(dscrptn); }
    }
}

[[maybe_unused]] void DeploymentThread::PrintMessage(const string& input)
{
    using namespace debugging_settings::DeploymentThread;
    if (disable_user_print_requests) { return; }

    // if input is multiline, split it into lines && print each line separately
    std::stringstream ss(input);
    std::string line;
    while (std::getline(ss, line)) { std::cout << clr::on_yellow << "[DPL] " << line << std::endl; }
}

bool DeploymentThread::load(const std::string& model_name_)
{

    // Creates the path depending on the OS
    std::string model_path_ = stripQuotes(std::string(MDL_path::default_model_path)) +
                              std::string(MDL_path::path_separator) +
                              model_name_;


    // If already tried the path, don't try again
    if (model_path == model_path_) {
        if (isModelLoaded) {
            return true;
        } else {
            return false;
        }
    }

    model_path = model_path_;

    ifstream myFile;
    myFile.open(model_path);
    if (myFile.is_open()) {
        cout << "Model file found at: " + model_path << " -- Trying to load model..." << endl;
        myFile.close();
        model = torch::jit::load(model_path, torch::kCPU);
        model.eval();
        isModelLoaded = true;
        myFile.close();
        return true;
    } else {
        cout << "Model file not found at: " + model_path << endl;
        isModelLoaded = false;
        return false;
    }
}

[[maybe_unused]] void DeploymentThread::DisplayTensor(const torch::Tensor &tensor, const string& Label,
                                     bool display_content=false){

    auto showMessage = [](const std::string& input) {
        // if input is multiline, split it into lines and print each line separately
        std::stringstream ss(input);
        std::string line;

        while (std::getline(ss, line)) {
            std::cout << clr::blue << "[MDL] " << line << std::endl;
        }
    };

    std::stringstream ss;
    ss << "TENSOR:" << Label ;
    ss << " | Tensor metadata: " ;
    ss << " | Device: " << tensor.device();
    ss << " | Size: " << tensor.sizes();
    ss << " |  - Storage data pointer: " << tensor.storage().data_ptr();

    if (display_content) {
        ss << " | Tensor content:" << std::endl;
        ss << tensor << std::endl;
    }

    showMessage(ss.str());
}