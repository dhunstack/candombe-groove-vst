
#pragma once
#include "Source/DeploymentThreads/DeploymentThread.h"
#include <juce_audio_basics/juce_audio_basics.h>

using namespace torch::indexing;

class PluginDeploymentThread: public DeploymentThread {
public:

    // initialize your deployment thread here
    PluginDeploymentThread():DeploymentThread() {

    }

    // tensors
    torch::Tensor hits = torch::zeros({1024, 32, 3}, torch::kFloat32);
    torch::Tensor velocities = torch::zeros({1024, 32, 3}, torch::kFloat32);
    torch::Tensor offsets = torch::zeros({1024, 32, 3}, torch::kFloat32);

    bool newPlaybackPolicyShouldBeSent = false;
    bool newPlaybackSequenceShouldBeSent = false;

    std::pair<bool, bool> deploy (
        std::optional<MidiFileEvent> & new_midi_event_dragdrop,
        std::optional<EventFromHost> & new_event_from_host,
        bool gui_params_changed_since_last_call,
        bool new_preset_loaded_since_last_call,
        bool new_midi_file_dropped_on_visualizers,
        bool new_audio_file_dropped_on_visualizers) override {

        newPlaybackPolicyShouldBeSent = false;
        newPlaybackSequenceShouldBeSent = false;

        // process midi dropped
        auto new_sequence_to_humanize = updateInputFromMidiFile();
        if (new_sequence_to_humanize) {
            midiVisualizersData->clear_visualizer_data("MIDI_Out");
        }

        // load model
        if (!isModelLoaded) {
            load("candombeEncoder.pt");
            model.eval();
        }

        // main deployment logic
        if (isModelLoaded) {

            // update the voice mapping
            updateVoiceMapping();

            if (gui_params.wasParamUpdated("Humanize!!") && new_sequence_to_humanize && num2BarSegments > 0)
            {
                // clear playbackSequence
                playbackSequence.clear();

                // print hits
                cout << "num2BarSegments: " << num2BarSegments << endl;

                // run inference
                // torch no grad
                torch::NoGradGuard no_grad;

                auto hit_in = hits.index({Slice(0, num2BarSegments), Slice(), Slice()});
                cout << "hit_in shape: " << hit_in.sizes() << endl;

                auto output = model.forward({hit_in});

                cout << "output: "  << endl;

                auto out_tuple = output.toTuple();
                cout << "out_tuple: " << endl;

                auto vel = out_tuple->elements()[1].toTensor();
                cout << "vel: " << endl;

                auto off = out_tuple->elements()[2].toTensor();
                cout << "off: "  << endl;

//                vel = torch::tanh(vel) / 2 + 0.5;
//                off = torch::tanh(off) / 2;

                vel = vel / 2 + 0.5;
                off = off / 2;


                // extract the velocities/offsets
                // find non-zero hit indices
                auto hit_indices = hits.index({Slice(0, num2BarSegments), Slice(0, 32), Slice(0, 3)}).nonzero();
                // cout << "hit_indices: " << hit_indices << endl;

                // iterate through the hits
                for (int i = 0; i < hit_indices.size(0); i++) {
                    auto batch_ix = hit_indices[i][0].item<int>();
                    auto grid_ix = hit_indices[i][1].item<int>();
                    auto voice_ix = hit_indices[i][2].item<int>();
                    auto real_time = batch_ix * 32 * 0.25f + grid_ix * 0.25f;

                    // update the velocities/offsets
                    velocities[batch_ix][grid_ix][voice_ix] = vel[batch_ix][grid_ix][voice_ix];
                    offsets[batch_ix][grid_ix][voice_ix] = off[batch_ix][grid_ix][voice_ix];
                    real_time = real_time + offsets[batch_ix][grid_ix][voice_ix].item<float>() * 0.25f / 2.0f;
                    if (real_time < 0.0f) { real_time += num2BarSegments * 32 * 0.25f; }

                    playbackSequence.addNoteWithDuration(10, display_voice_maps[voice_ix], vel[batch_ix][grid_ix][voice_ix].item<float>(), real_time, 0.1f);
                    midiVisualizersData->displayNoteWithDuration("MIDI_Out", display_voice_maps[voice_ix], vel[batch_ix][grid_ix][voice_ix].item<float>(), real_time, 0.1f);
                }


                // prepare the playback sequence and policy
//                preparePlaybackSequence();
                preparePlaybackPolicy();

                // set the flags
                newPlaybackPolicyShouldBeSent = true;
                newPlaybackSequenceShouldBeSent = true;
            }

            // uncheck the humanize button
            gui_params.setValueFor("Humanize!!", 0.0f);
        }


        // your implementation goes here
        return {newPlaybackPolicyShouldBeSent, newPlaybackSequenceShouldBeSent};
    }

private:


    std::map<int, int> voiceMap;
    int num2BarSegments = 0;

    // voice map for visualization purposes only
    map<int, int> display_voice_maps = {
        {0, 36}, {1, 38}, {2, 40}
    };

    map<int, int> pitch2ix = {
        {36, 0}, {38, 1}, {40, 2}
    };

    bool updateInputFromMidiFile() {
        // check if a midi file has been dropped on the visualizers
        auto new_sequence = midiVisualizersData->get_visualizer_data("MIDI_In");

        // return false if no new sequence is available
        if (new_sequence == std::nullopt) {
            return false;
        }

        num2BarSegments = 0;
        hits.fill_(0.0);
        velocities.fill_(0.0);
        offsets.fill_(0.0);

        // clear out existing groove input
        for (const auto &event: *new_sequence) {
            if (event.isNoteOnEvent()) {
                auto pitch = event.getNoteNumber();
                if (pitch2ix.find(pitch) == pitch2ix.end()) {
                    continue;
                } else {
                    auto ix = pitch2ix[pitch];
                    // PrintMessage(event.getDescription().str());
                    auto ppq = event.Time(); // time in ppq
                    // auto velocity = event.getVelocity(); // velocity
                    auto div = round(ppq / .25f);
                    // auto offset = (ppq - (div * .25f)) / 0.125 * 0.5;
                    auto batch_index = (long long) div / 32;
                    auto grid_index = (long long) fmod(div, 32);
                    hits[batch_index][grid_index][ix] = 1.0;
                    num2BarSegments = std::max(num2BarSegments, (int) batch_index+1);

                }
            }
        }

        return true;
    }

    // playback policy
    torch::Tensor playback_policy;



    // update per voice midi mapping
    void updateVoiceMapping() {
        bool changed = false;

        // --------------------- Voice Map ---------------------
        if (gui_params.wasParamUpdated("Chico")) {
            voiceMap[0] = int(gui_params.getValueFor("Chico"));
            changed = true;
        }
        if (gui_params.wasParamUpdated("Piano")) {
            voiceMap[1] = int(gui_params.getValueFor("Piano"));
            changed = true;
        }
        if (gui_params.wasParamUpdated("Repique")) {
            voiceMap[2] = int(gui_params.getValueFor("Repique"));
            changed = true;
        }

        newPlaybackSequenceShouldBeSent = changed || newPlaybackSequenceShouldBeSent;

    }

    // extracts the generated pattern into a PlaybackSequence
    void preparePlaybackSequence() {
//        if (!playback_hits.sizes().empty()) // check if any hits are available
//        {
//            // clear playback sequence
//            playbackSequence.clear();
//
//
//
//            // iterate through all voices, and time steps
//            int batch_ix = 0;
//            for (int step_ix = 0; step_ix < playback_hits.size(1); step_ix++)
//            {
//                for (int voice_ix = 0; voice_ix < 9; voice_ix++)
//                {
//
//                    // check if the voice is active at this time step
//                    if (playback_hits[batch_ix][step_ix][voice_ix].item<float>() > 0.5)
//                    {
//                        auto midi_num = voiceMap[voice_ix];
//                        auto velocity = playback_velocities[batch_ix][step_ix][voice_ix].item<float>();
//                        if (velocity >= generation_vel_thresh) {
//                            velocity = std::clamp(velocity, 0.0f, 1.0f);
//                            auto offset =
//                                playback_offsets[batch_ix][step_ix][voice_ix].item<float>() * (1.0 - generation_off_quantizer_val);
//                            // we are going to convert the onset time to a ratio of quarter notes
//                            auto time = (step_ix + offset) * 0.25f;
//                            auto duration = 0.1f;
//
//                            if (time < 0.0f) { time += 4.0;}
//                            if (velocity > 0.0f) {
//                                playbackSequence.addNoteWithDuration(
//                                    10, midi_num, velocity, time, duration);
//                            }
//                        }
//                    }
//                }
//            }
//        }
    }

    // prepares the playback policy
    void preparePlaybackPolicy() {
//        // Specify the playback policy
        playbackPolicy.SetPlaybackPolicy_RelativeToAbsoluteZero();
        playbackPolicy.SetTimeUnitIsPPQ();
        playbackPolicy.SetOverwritePolicy_DeleteAllEventsInPreviousStreamAndUseNewStream(true);
        auto Range = num2BarSegments * 8;
        playbackPolicy.ActivateLooping(Range);
    }
};