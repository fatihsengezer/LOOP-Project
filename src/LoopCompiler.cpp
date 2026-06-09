/**
 * @file    LoopCompiler.cpp
 * @brief   A command-line tool to compile WAV files and a JSON sequence definition into a .LOOP binary file.
 */

#include "../include/FormatSpec.h"
#include "../include/LoopFileWriter.h"

#include <juce_core/juce_core.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <iostream>
#include <map>
#include <string>

void printUsage()
{
    std::cout << "LOOP Compiler Utility\n";
    std::cout << "Usage: LOOPCompiler <config.json> <output.loop>\n";
    std::cout << "Example: LOOPCompiler project.json output.loop\n";
}

int main (int argc, char* argv[])
{
    if (argc < 3)
    {
        printUsage();
        return 1;
    }

    const juce::File configFile (juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]));
    const juce::File outputFile (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]));

    if (! configFile.existsAsFile())
    {
        std::cerr << "Error: Configuration file not found: " << configFile.getFullPathName() << "\n";
        return 1;
    }

    // Parse JSON config
    juce::var config;
    auto parseResult = juce::JSON::parse (configFile.loadFileAsString(), config);
    if (parseResult.failed())
    {
        std::cerr << "Error parsing JSON: " << parseResult.getErrorMessage() << "\n";
        return 1;
    }

    auto* obj = config.getDynamicObject();
    if (obj == nullptr)
    {
        std::cerr << "Error: Top-level JSON must be an object.\n";
        return 1;
    }

    LoopFormat::LoopFileWriter writer;

    // Time signature and metadata
    const int tsNum = obj->getProperty ("time_signature_num").isVoid() ? 4 : (int) obj->getProperty ("time_signature_num");
    const int tsDen = obj->getProperty ("time_signature_den").isVoid() ? 4 : (int) obj->getProperty ("time_signature_den");
    writer.setTimeSignature (static_cast<uint16_t> (tsNum), static_cast<uint8_t> (tsDen));

    if (obj->hasProperty ("project"))
        writer.addMeta ("project", obj->getProperty ("project").toString().toStdString());
    if (obj->hasProperty ("author"))
        writer.addMeta ("author", obj->getProperty ("author").toString().toStdString());

    // Register audio formats
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    // Map to keep track of loaded sample ID mappings (JSON index/name -> assigned internal ID)
    std::map<std::string, uint16_t> sampleMapByName;
    std::map<int, uint16_t> sampleMapById;

    // Load samples
    const auto* samplesArray = obj->getProperty ("samples").getArray();
    if (samplesArray == nullptr)
    {
        std::cerr << "Error: Configuration must contain a 'samples' array.\n";
        return 1;
    }

    std::cout << "Loading samples...\n";
    for (int i = 0; i < samplesArray->size(); ++i)
    {
        const auto& sampleVar = samplesArray->getReference (i);
        auto* sampleObj = sampleVar.getDynamicObject();
        if (sampleObj == nullptr)
        {
            std::cerr << "Error: Sample entry " << i << " is not a valid object.\n";
            return 1;
        }

        const int jsonId = sampleObj->getProperty ("id").isVoid() ? i : (int) sampleObj->getProperty ("id");
        const juce::String sampleName = sampleObj->getProperty ("name").toString();
        const juce::String relativePath = sampleObj->getProperty ("path").toString();

        if (relativePath.isEmpty())
        {
            std::cerr << "Error: Sample entry " << i << " is missing 'path'.\n";
            return 1;
        }

        // Try to find the file relative to the config file directory
        juce::File wavFile = configFile.getParentDirectory().getChildFile (relativePath);
        if (! wavFile.existsAsFile())
        {
            // Fallback to relative to current working directory
            wavFile = juce::File::getCurrentWorkingDirectory().getChildFile (relativePath);
        }

        if (! wavFile.existsAsFile())
        {
            std::cerr << "Error: Audio file not found: " << relativePath << "\n";
            return 1;
        }

        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (wavFile));
        if (reader == nullptr)
        {
            std::cerr << "Error: Failed to read audio file format (unsupported): " << wavFile.getFullPathName() << "\n";
            return 1;
        }

        juce::AudioBuffer<float> buffer (static_cast<int> (reader->numChannels), static_cast<int> (reader->lengthInSamples));
        if (! reader->read (&buffer, 0, static_cast<int> (reader->lengthInSamples), 0, true, true))
        {
            std::cerr << "Error: Failed to read PCM data from: " << wavFile.getFullPathName() << "\n";
            return 1;
        }

        LoopFormat::SampleMeta meta;
        meta.sample_rate = static_cast<uint32_t> (reader->sampleRate);
        meta.channels    = static_cast<uint8_t> (reader->numChannels);
        meta.bit_depth   = static_cast<uint8_t> (reader->bitsPerSample == 24 ? 24 : 16); // force 16 or 24-bit
        meta.base_note   = sampleObj->getProperty ("base_note").isVoid() ? 60.f : static_cast<float> (double (sampleObj->getProperty ("base_note")));
        meta.loop_bpm    = sampleObj->getProperty ("loop_bpm").isVoid() ? 0.f : static_cast<float> (double (sampleObj->getProperty ("loop_bpm")));
        meta.name        = sampleName.toStdString();

        uint16_t assignedId = 0;
        auto addErr = writer.addSample (buffer, meta, &assignedId);
        if (addErr != LoopFormat::LoopFileWriter::Error::None)
        {
            std::cerr << "Error adding sample: " << LoopFormat::LoopFileWriter::describeError (addErr) << "\n";
            return 1;
        }

        std::cout << "  Loaded '" << sampleName << "' -> assigned ID: " << assignedId << " (" 
                  << reader->numChannels << "ch, " << reader->sampleRate << "Hz, " 
                  << reader->bitsPerSample << "-bit, trimmed to " << buffer.getNumSamples() << " samples)\n";

        sampleMapById[jsonId] = assignedId;
        if (sampleName.isNotEmpty())
            sampleMapByName[sampleName.toStdString()] = assignedId;
    }

    // Load events
    const auto* eventsArray = obj->getProperty ("events").getArray();
    if (eventsArray == nullptr)
    {
        std::cerr << "Error: Configuration must contain an 'events' array.\n";
        return 1;
    }

    std::cout << "Parsing events...\n";
    for (int i = 0; i < eventsArray->size(); ++i)
    {
        const auto& eventVar = eventsArray->getReference (i);
        auto* eventObj = eventVar.getDynamicObject();
        if (eventObj == nullptr)
        {
            std::cerr << "Error: Event entry " << i << " is not a valid object.\n";
            return 1;
        }

        const uint32_t tick = static_cast<uint32_t> (int (eventObj->getProperty ("tick")));
        const uint8_t velocity = static_cast<uint8_t> (int (eventObj->getProperty ("velocity").isVoid() ? 100 : int (eventObj->getProperty ("velocity"))));
        const uint32_t duration = static_cast<uint32_t> (int (eventObj->getProperty ("duration").isVoid() ? 960 : int (eventObj->getProperty ("duration"))));
        const bool oneShot = static_cast<bool> (eventObj->getProperty ("one_shot").isVoid() ? true : bool (eventObj->getProperty ("one_shot")));

        // Find correct sample ID mapping
        uint16_t sampleId = 0;
        if (eventObj->hasProperty ("sample_id"))
        {
            const auto sampleIdProp = eventObj->getProperty ("sample_id");
            if (sampleIdProp.isString())
            {
                const auto nameStr = sampleIdProp.toString().toStdString();
                if (sampleMapByName.count (nameStr))
                {
                    sampleId = sampleMapByName[nameStr];
                }
                else
                {
                    std::cerr << "Error: Event entry " << i << " references unknown sample name: " << nameStr << "\n";
                    return 1;
                }
            }
            else
            {
                const int idInt = (int) sampleIdProp;
                if (sampleMapById.count (idInt))
                {
                    sampleId = sampleMapById[idInt];
                }
                else
                {
                    std::cerr << "Error: Event entry " << i << " references unknown sample ID: " << idInt << "\n";
                    return 1;
                }
            }
        }
        else if (eventObj->hasProperty ("sample_name"))
        {
            const auto nameStr = eventObj->getProperty ("sample_name").toString().toStdString();
            if (sampleMapByName.count (nameStr))
            {
                sampleId = sampleMapByName[nameStr];
            }
            else
            {
                std::cerr << "Error: Event entry " << i << " references unknown sample name: " << nameStr << "\n";
                return 1;
            }
        }
        else
        {
            std::cerr << "Error: Event entry " << i << " must have 'sample_id' or 'sample_name'.\n";
            return 1;
        }

        auto addErr = writer.addEvent (tick, sampleId, velocity, duration, oneShot);
        if (addErr != LoopFormat::LoopFileWriter::Error::None)
        {
            std::cerr << "Error adding event: " << LoopFormat::LoopFileWriter::describeError (addErr) << "\n";
            return 1;
        }
    }

    std::cout << "Compiling to " << outputFile.getFullPathName() << "...\n";
    auto writeErr = writer.write (outputFile);
    if (writeErr != LoopFormat::LoopFileWriter::Error::None)
    {
        std::cerr << "Compilation failed: " << LoopFormat::LoopFileWriter::describeError (writeErr) << "\n";
        return 1;
    }

    std::cout << "Success! Compiled " << writer.estimateOutputBytes() << " bytes.\n";
    return 0;
}
