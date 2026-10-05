/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin processor.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
// Settings file: RRV10/RRV10.settings in the user's application data folder (%APPDATA% on Windows,
// ~/Library/Application Support on macOS), a JUCE <PROPERTIES> file with camelCase keys. It is created with
// the defaults when missing, missing keys are added, and the values are read every time the host prepares
// the plugin, so an edit takes effect the next time the plugin is loaded or the audio device is restarted.
//
//   emulationRate     "native" (default): the chip runs at nativeSampleRate, like the hardware, with the
//                     host signal converted to that rate and back. Decay, pre-delay and tone are the same at
//                     any host rate.
//                     "host": the chip runs once per host sample, as in earlier versions of this plugin. The
//                     reverb then gets shorter and brighter as the host rate rises (1.41x at 44.1 kHz,
//                     3.07x at 96 kHz).
//   nativeSampleRate  the chip rate in Hz for "native" mode. The RRV-10 runs at 31250 Hz (8 MHz crystal,
//                     256 cycles per sample). Other values speed the reverb up or slow it down, like a
//                     detuned crystal. Limited to 8000 - 96000.

void ReverbAudioProcessor::loadSettings() {
  juce::PropertiesFile::Options o;
  o.applicationName = "RRV10";
  o.folderName = "RRV10";
  o.filenameSuffix = ".settings";
  o.osxLibrarySubFolder = "Application Support";
  o.storageFormat = juce::PropertiesFile::storeAsXML;
  o.millisecondsBeforeSaving = -1; // only save when asked

  juce::PropertiesFile settings(o);

  bool changed = false;
  if (!settings.containsKey("emulationRate")) {
    settings.setValue("emulationRate", "native");
    changed = true;
  }
  if (!settings.containsKey("nativeSampleRate")) {
    settings.setValue("nativeSampleRate", 31250);
    changed = true;
  }
  if (changed)
    settings.saveIfNeeded();

  useNativeRate =
      settings.getValue("emulationRate", "native").trim().compareIgnoreCase("host") != 0;
  nativeSampleRate =
      juce::jlimit(8000.0, 96000.0, settings.getDoubleValue("nativeSampleRate", 31250.0));
}

ReverbAudioProcessor::ReverbAudioProcessor()
    : AudioProcessor(
          BusesProperties()
              .withInput("Input", juce::AudioChannelSet::stereo(), true)
              .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      bossEmu((unsigned char *)BinaryData::rrv10_bin, BinaryData::rrv10_binSize,
              BossEmu::RV_2_EMU_MODE) {

  addParameter(enabled = new juce::AudioParameterBool(
                   juce::ParameterID{"enabled", 1}, // parameterID
                   "Enabled",                 // parameter name
                   true));                    // default value
  addParameter(effectLevel = new juce::AudioParameterFloat(
                   juce::ParameterID{"effectLevel", 1}, // parameterID
                   "Effect Level",                // parameter name
                   0.0f,                          // minimum value
                   1.0f,                          // maximum value
                   0.4f));                        // default value
  addParameter(directLevel = new juce::AudioParameterFloat(
                   juce::ParameterID{"directLevel", 1}, // parameterID
                   "Direct Level",                // parameter name
                   0.0f,                          // minimum value
                   1.0f,                          // maximum value
                   1.0f));                        // default value
  addParameter(
      mode = new juce::AudioParameterInt(juce::ParameterID{"mode", 1}, // parameterID
                                         "Mode", // parameter name
                                         0,      // minimum value
                                         8,      // maximum value
                                         0));    // default value
  addParameter(decayTime = new juce::AudioParameterFloat(
                   juce::ParameterID{"decayTime", 1}, // parameterID
                   "Decay Time",                // parameter name
                   0.0f,                        // minimum value
                   15.0f,                       // maximum value
                   5.0f));                      // default value
  addParameter(preEq = new juce::AudioParameterFloat(
                   juce::ParameterID{"preEq", 1}, // parameterID
                   "Pre Eq",                // parameter name
                   0.0f,                    // minimum value
                   1.0f,                    // maximum value
                   0.5f));                  // default value

  enabled->addListener(this);
  effectLevel->addListener(this);
  directLevel->addListener(this);
  mode->addListener(this);
  decayTime->addListener(this);
  preEq->addListener(this);
}

ReverbAudioProcessor::~ReverbAudioProcessor() {}

//==============================================================================
void ReverbAudioProcessor::parameterValueChanged(int parameterIndex,
                                                 float newValue) {
  sendChangeMessage();

  if (parameterIndex == mode->getParameterIndex() || parameterIndex == decayTime->getParameterIndex()) {
    emuLock.enter();
    bossEmu.setParameters(*mode, *decayTime, 7);
    emuLock.exit();
  }
}

void ReverbAudioProcessor::parameterGestureChanged(int parameterIndex,
                                                   bool gestureIsStarting) {}

//==============================================================================
const juce::String ReverbAudioProcessor::getName() const {
  return JucePlugin_Name;
}

bool ReverbAudioProcessor::acceptsMidi() const { return false; }

bool ReverbAudioProcessor::producesMidi() const { return false; }

bool ReverbAudioProcessor::isMidiEffect() const { return false; }

double ReverbAudioProcessor::getTailLengthSeconds() const {
  // TODO
  return 0.0;
}

int ReverbAudioProcessor::getNumPrograms() {
  return 1; // NB: some hosts don't cope very well if you tell them there are 0
            // programs, so this should be at least 1, even if you're not really
            // implementing programs.
}

int ReverbAudioProcessor::getCurrentProgram() { return 0; }

void ReverbAudioProcessor::setCurrentProgram(int index) {}

const juce::String ReverbAudioProcessor::getProgramName(int index) {
  return {};
}

void ReverbAudioProcessor::changeProgramName(int index,
                                             const juce::String &newName) {}

//==============================================================================
void ReverbAudioProcessor::prepareToPlay(double sampleRate,
                                         int samplesPerBlock) {
  loadSettings();

  const auto size = (size_t)juce::jmax(samplesPerBlock, 1);
  chipInL.assign(size, 0.0f);
  chipInR.assign(size, 0.0f);
  chipOutL.assign(size, 0.0f);
  chipOutR.assign(size, 0.0f);

  emuLock.enter();
  bossEmu.reset();
  bossEmu.setParameters(*mode, *decayTime, 7);
  nativeRunner.prepare(sampleRate, nativeSampleRate);
  emuLock.exit();
}

void ReverbAudioProcessor::processChip(const float *inL, const float *inR,
                                       float *outL, float *outR, int n) {
  static constexpr float scaleFactor = 16383.0f;

  auto chipSample = [this](float l, float r, float &ol, float &orr) {
    short inLeft = (short)juce::jlimit(-32768.0f, 32767.0f, l * scaleFactor);
    short inRight = (short)juce::jlimit(-32768.0f, 32767.0f, r * scaleFactor);
    short outLeft, outRight;
    bossEmu.process(&inLeft, &inRight, &outLeft, &outRight, 1);
    ol = outLeft / scaleFactor;
    orr = outRight / scaleFactor;
  };

  emuLock.enter();
  if (useNativeRate)
    nativeRunner.run(inL, inR, outL, outR, n, chipSample);
  else
    for (int i = 0; i < n; ++i)
      chipSample(inL[i], inR[i], outL[i], outR[i]);
  emuLock.exit();
}

void ReverbAudioProcessor::releaseResources() {
  // When playback stops, you can use this as an opportunity to free up any
  // spare memory, etc.
}

bool ReverbAudioProcessor::isBusesLayoutSupported(
    const BusesLayout &layouts) const {
  // This is the place where you check if the layout is supported.
  // In this template code we only support mono or stereo.
  // Some plugin hosts, such as certain GarageBand versions, will only
  // load plugins that support stereo bus layouts.
  if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
    return false;

  // This checks if the input layout matches the output layout
  if (layouts.getMainInputChannelSet() != layouts.getMainInputChannelSet())
    return false;

  return true;
}

void ReverbAudioProcessor::processBlock(juce::AudioBuffer<float> &buffer,
                                        juce::MidiBuffer &midiMessages) {
  juce::ScopedNoDenormals noDenormals;
  auto totalNumInputChannels = getTotalNumInputChannels();
  auto totalNumOutputChannels = getTotalNumOutputChannels();

  // In case we have more outputs than inputs, this code clears any output
  // channels that didn't contain input data, (because these aren't
  // guaranteed to be empty - they may contain garbage).
  // This is here to avoid people getting screaming feedback
  // when they first compile a plugin, but obviously you don't need to keep
  // this code if your algorithm always overwrites all the output channels.
  for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
    buffer.clear(i, 0, buffer.getNumSamples());

  auto *channelDataL = buffer.getWritePointer(0);
  auto *channelDataR = buffer.getWritePointer(1);

  bool prev_isOverloading = isOverloading;
  isOverloading = false;

  const int numSamples = buffer.getNumSamples();
  if ((size_t)numSamples > chipInL.size()) {
    // the host sent a bigger block than announced
    chipInL.resize((size_t)numSamples);
    chipInR.resize((size_t)numSamples);
    chipOutL.resize((size_t)numSamples);
    chipOutR.resize((size_t)numSamples);
  }

  for (int i = 0; i < numSamples; i++) {
    float drySampleL = channelDataL[i];
    float drySampleR = channelDataR[i];

    if (fabsf(drySampleL) > 1.3f || fabsf(drySampleR) > 1.3f) {
      isOverloading = true;
    }

    float filteredSampleL = drySampleL;
    float filteredSampleR = drySampleR;
    if (*preEq < 0.5f) {
      // Low-pass filter
      float cutoff = 1.0f - (0.5f - *preEq) * 2.0f;
      filteredSampleL = filterTempL + cutoff * (drySampleL - filterTempL);
      filteredSampleR = filterTempR + cutoff * (drySampleR - filterTempR);
    } else if (*preEq > 0.5f) {
      // High-pass filter
      float cutoff = (*preEq - 0.5f) * 2.0f;
      filteredSampleL =
          drySampleL - (filterTempL + cutoff * (drySampleL - filterTempL));
      filteredSampleR =
          drySampleR - (filterTempR + cutoff * (drySampleR - filterTempR));
    }
    filterTempL = filteredSampleL;
    filterTempR = filteredSampleR;

    chipInL[(size_t)i] = filteredSampleL;
    chipInR[(size_t)i] = filteredSampleR;
  }

  processChip(chipInL.data(), chipInR.data(), chipOutL.data(), chipOutR.data(),
              numSamples);

  if (*enabled) {
    for (int i = 0; i < numSamples; i++) {
      channelDataL[i] = chipOutL[(size_t)i] * *effectLevel + channelDataL[i] * *directLevel;
      channelDataR[i] = chipOutR[(size_t)i] * *effectLevel + channelDataR[i] * *directLevel;
    }
  }

  if (isOverloading != prev_isOverloading) {
    sendChangeMessage();
  }
}

//==============================================================================
bool ReverbAudioProcessor::hasEditor() const {
  return true; // (change this to false if you choose to not supply an editor)
}

juce::AudioProcessorEditor *ReverbAudioProcessor::createEditor() {
  return new ReverbAudioProcessorEditor(*this);
}

//==============================================================================
void ReverbAudioProcessor::getStateInformation(juce::MemoryBlock &destData) {
  std::unique_ptr<juce::XmlElement> xml(new juce::XmlElement("RRV10"));
  xml->setAttribute("enabled", (bool)*enabled);
  xml->setAttribute("effectLevel", (double)*effectLevel);
  xml->setAttribute("directLevel", (double)*directLevel);
  xml->setAttribute("mode", (int)*mode);
  xml->setAttribute("decayTime", (double)*decayTime);
  xml->setAttribute("preEq", (double)*preEq);
  copyXmlToBinary(*xml, destData);
}

void ReverbAudioProcessor::setStateInformation(const void *data,
                                               int sizeInBytes) {
  std::unique_ptr<juce::XmlElement> xmlState(
      getXmlFromBinary(data, sizeInBytes));

  if (xmlState.get() != nullptr) {
    if (xmlState->hasTagName("RRV10")) {
      *enabled = (bool)xmlState->getBoolAttribute("enabled", true);
      *effectLevel = (float)xmlState->getDoubleAttribute("effectLevel", 0.4f);
      *directLevel = (float)xmlState->getDoubleAttribute("directLevel", 1.0f);
      *mode = (int)xmlState->getIntAttribute("mode", 0);
      *decayTime = (double)xmlState->getIntAttribute("decayTime", 5.0f);
      *preEq = (float)xmlState->getDoubleAttribute("preEq", 0.5f);
    }
  }

  emuLock.enter();
  bossEmu.reset();
  bossEmu.setParameters(*mode, *decayTime, 7);
  emuLock.exit();

  sendChangeMessage();
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter() {
  return new ReverbAudioProcessor();
}
