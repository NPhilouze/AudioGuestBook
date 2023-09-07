#include <Keypad.h>
#include <Bounce.h>
#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <MTP_Teensy.h>
#include "play_sd_wav.h" // local copy with fixes

AudioSynthWaveform          waveform1; // To create the "beep" sfx
AudioInputI2S               i2s2; // I2S input from microphone on audio shield
AudioPlaySdWavX             playWav1; // Play 44.1kHz 16-bit PCM greeting WAV file
AudioRecordQueue            queue1; // Creating an audio buffer in memory before saving to SD
AudioMixer4                 mixer; // Allows merging several inputs to same output
AudioOutputI2S              i2s1; // I2S interface to Speaker/Line Out on Audio shield
AudioConnection patchCord1(waveform1, 0, mixer, 0); // wave to mixer 
AudioConnection patchCord2(playWav1, 0, mixer, 1); // wav file playback mixer
AudioConnection patchCord3(mixer, 0, i2s1, 0); // mixer output to speaker (L)
// AudioConnection patchCord4(mixer, 0, i2s1, 1); // mixer output to speaker (R) SOUND TO RINGER
AudioConnection patchCord5(i2s2, 0, queue1, 0); // mic input to queue (L)
AudioControlSGTL5000      sgtl5000_1;

// Filename to save audio recording on SD card
char filename[15];
// The file object itself
File frec;

// Keep track of current state of the device
enum Mode {Initialising, Ready, Prompting, Recording};
Mode mode = Mode::Initialising;

float beep_volume_low = 0.1f; // Lower volume for when receiver is on ear
float beep_volume = 1.0f;

uint32_t MTPcheckInterval; // default value of device check interval [ms]

// variables for writing to WAV file
unsigned long ChunkSize = 0L;
unsigned long Subchunk1Size = 16;
unsigned int AudioFormat = 1;
unsigned int numChannels = 1;
unsigned long sampleRate = 44100;
unsigned int bitsPerSample = 16;
unsigned long byteRate = sampleRate*numChannels*(bitsPerSample/8);// samplerate x channels x (bitspersample / 8)
unsigned int blockAlign = numChannels*bitsPerSample/8;
unsigned long Subchunk2Size = 0L;
unsigned long recByteSaved = 0L;
unsigned long NumSamples = 0L;
byte byte1, byte2, byte3, byte4;

// Mic
const int myInput = AUDIO_INPUT_MIC;

// SD Card
#define SDCARD_CS_PIN    BUILTIN_SDCARD
#define SDCARD_MOSI_PIN  11  // not actually used
#define SDCARD_SCK_PIN   13  // not actually used

// Keypad
const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','R'}
};
byte rowPins[ROWS] = {27, 26, 25, 24}; //connect to the row pinouts of the keypad
byte colPins[COLS] = {31, 30, 29, 28}; //connect to the column pinouts of the keypad

Keypad keypad = Keypad( makeKeymap(keys), rowPins, colPins, ROWS, COLS );

// Hook
#define HOOK_PIN 41
Bounce hook = Bounce(HOOK_PIN, 40);

char instructionsLang[3] = "EN";

// Debugging: Change to INSTRUMENT_SD_WRITE
#define noINSTRUMENT_SD_WRITE

// Methods default values
void end_Beep(int beeps=4, float volume=beep_volume_low);

void setup(){
  Serial.begin(9600);
  while (!Serial && millis() < 5000) {
    // wait for serial port to connect.
  }
  Serial.println("Serial set up correctly");
  Serial.printf("Audio block set to %d samples\n",AUDIO_BLOCK_SAMPLES);
  print_mode();

  pinMode(HOOK_PIN, INPUT_PULLUP); // Hook

  // Audio connections require memory, and the record queue
  // uses this memory to buffer incoming audio.
  AudioMemory(60);

  // Enable the audio shield, select input, and enable output
  sgtl5000_1.enable();
  sgtl5000_1.inputSelect(myInput);
  sgtl5000_1.volume(0.95);

  // Initialize the SD card
  SPI.setMOSI(SDCARD_MOSI_PIN);
  SPI.setSCK(SDCARD_SCK_PIN);
  if (!(SD.begin(SDCARD_CS_PIN))) {
    // stop here if no SD card, but print a message
    while (1) {
      Serial.println("Unable to access the SD card");
      delay(500);
    }
  } else {
    Serial.println("SD card correctly initialized");
  }

  // Play a startup sound to indicate system is online
  mixer.gain(0, 3.0f);
  playWav1.play("startup.wav");
  while (!playWav1.isStopped()) {
    // Wait until file is done playing before continuing
  }
  mixer.gain(0, 0.3f);

  // mandatory to begin the MTP session.
  MTP.begin();

  // Add SD Card
  MTP.addFilesystem(SD, "Audio guestbook"); // choose a nice name for the SD card volume to appear in your file explorer
  Serial.println("Added SD card via MTP");
  MTPcheckInterval = MTP.storage()->get_DeltaDeviceCheckTimeMS();
  
  sgtl5000_1.micGain(25); // in DB
 
  mode = Mode::Ready; print_mode();
}
  
void loop(){
  // Keypad
  char key = keypad.getKey();
  if (key != NO_KEY){
    Serial.println(key);
  }

  // Hook
  hook.update();

  switch(mode){
    case Mode::Ready:
      // Falling edge occurs when the handset is lifted
      if (hook.fallingEdge()) {
        Serial.println("Handset lifted");
        if (!SD.exists("greeting.wav")) {
          Serial.println("greeting.wav missing");
          wait(1000);
          help();
        } else {
          mode = Mode::Prompting; print_mode();
        }
      } else if (key == 'R') {
        wait(1000);
        playLastRecording();
      } else if (key == '0') {
        wait(1000);
        help();
      } else if (key == '1') {
        wait(1000);
        playAllRecordings();
      }
      break;

    case Mode::Prompting:
      // Wait a second for users to put the handset to their ear
      wait(1000);
      // Play the greeting inviting them to record their message
      playWav1.play("greeting.wav");    
      // Wait until the  message has finished playing
      while (!playWav1.isStopped()) {
        hook.update();
        char key = keypad.getKey();

        // Handset is replaced
        if(hook.risingEdge()) {
          playWav1.stop();
          mode = Mode::Ready; print_mode();
          return;
        } else if (key == 'R') {
          playWav1.stop();
          playLastRecording();
          return;
        } else if (key == '0') {
          playWav1.stop();
          help();
          return;
        } else if (key == '1') {
          playWav1.stop();
          playAllRecordings();
          return;
        }
      }
      // Debug message
      Serial.println("Starting Recording");
      // Play the tone sound effect
      waveform1.begin(beep_volume_low, 440, WAVEFORM_SINE);
      if (wait(1000)) {
        waveform1.amplitude(0);
        startRecording();
      } else {
        waveform1.amplitude(0);
        mode = Mode::Ready; print_mode();
        return;
      }
      break;

    case Mode::Recording:
      // Handset is replaced
      if(hook.risingEdge() || key == '#'){
        // Debug log
        Serial.println("Stopping Recording");
        // Stop recording
        stopRecording();
        // Play audio tone to confirm recording has ended
        if (key == '#') {
          end_Beep();
        } else {
          end_Beep(4, beep_volume);
        }
      }
      else {
        continueRecording();
      }
      break; 

    case Mode::Initialising: // to make compiler happy
      break;  
  }   
  
  MTP.loop();  // This is mandatory to be placed in the loop code.
}

void setMTPdeviceChecks(bool nable)
{
  if (nable)
  {
    MTP.storage()->set_DeltaDeviceCheckTimeMS(MTPcheckInterval);
    Serial.print("En");
  }
  else
  {
    MTP.storage()->set_DeltaDeviceCheckTimeMS((uint32_t) -1);
    Serial.print("Dis");
  }
  Serial.println("abled MTP storage device checks");
}

#if defined(INSTRUMENT_SD_WRITE)
static uint32_t worstSDwrite, printNext;
#endif // defined(INSTRUMENT_SD_WRITE)

void help() {
  Serial.println("Playing help lang prompt");
  playWav1.play("helpLangPrompt.wav");

  // Lang prompt
  while (!playWav1.isStopped()) {
    hook.update();
    char key = keypad.getKey();
    if(hook.risingEdge()) {
      playWav1.stop();
      mode = Mode::Ready; print_mode();
      return;
    } else if(key == '1' || key == '2') {
      if (key == '1') {
        strcpy(instructionsLang, "EN");
        Serial.println("Playing help prompt in EN");
        playWav1.play("helpPromptEN.wav");
      } else {
        strcpy(instructionsLang, "FR");
        Serial.println("Playing help prompt in FR");
        playWav1.play("helpPromptFR.wav");
      }
      
      // Instructions prompt
      while (!playWav1.isStopped()) {
        hook.update();
        char key = keypad.getKey();
        if(hook.risingEdge()) {
          playWav1.stop();
          mode = Mode::Ready; print_mode();
          return;
        } else if(key == '1' || key == '2') {
          if (key == '1') {
            if (strcmp(instructionsLang, "EN") == 0) {
              Serial.println("Prompting for new greeting message in EN");
              playWav1.play("greetingInstructionsEN.wav");
            } else {
              Serial.println("Prompting for new greeting message in FR");
              playWav1.play("greetingInstructionsFR.wav");
            }
            while (!playWav1.isStopped()) {
              hook.update();
              // Handset is replaced
              if(hook.risingEdge()) {
                playWav1.stop();
                mode = Mode::Ready; print_mode();
                return;
              }
            }

            // Play the tone sound effect
            waveform1.begin(beep_volume_low, 440, WAVEFORM_SINE);
            if (wait(1000)) {
              waveform1.amplitude(0);
              SD.remove("greeting.wav");
              startRecording();
              return;
            } else {
              waveform1.amplitude(0);
              mode = Mode::Ready; print_mode();
              return;
            }
          } else {
            if (strcmp(instructionsLang, "EN") == 0) {
              Serial.println("Playing instructions in EN");
              playWav1.play("instructionsEN.wav");
            } else {
              Serial.println("Playing instructions in FR");
              playWav1.play("instructionsFR.wav");
            }         

            // Instructions
            while (!playWav1.isStopped()) {
              hook.update();
              if(hook.risingEdge()) {
                playWav1.stop();
                mode = Mode::Ready; print_mode();
                return;
              }
            }
          }
        }
      }
    }
  }
  // file has been played
  mode = Mode::Ready; print_mode();  
  end_Beep();
}

void startRecording() {
  setMTPdeviceChecks(false); // disable MTP device checks while recording
  #if defined(INSTRUMENT_SD_WRITE)
    worstSDwrite = 0;
    printNext = 0;
  #endif // defined(INSTRUMENT_SD_WRITE)
  if (!SD.exists("greeting.wav")) {
    strcpy(filename, "greeting.wav");
  } else {
    uint16_t idx = 0;
    for (uint16_t i=500; i>=0; i--) {
      // Format the counter as a five-digit number with leading zeroes, followed by file extension
      snprintf(filename, 11, " %05d.wav", i);
      // check, if file with index i exists
      if (SD.exists(filename)) {
        idx = i + 1;
        break;
      } else if (i == 0) {
        break;
      }
    }
    snprintf(filename, 11, " %05d.wav", idx);
  }
  Serial.print("Filename: ");
  Serial.println(filename);
  frec = SD.open(filename, FILE_WRITE);
  Serial.println("Opened file !");
  if (frec) {
    Serial.print("Recording to ");
    Serial.println(filename);
    queue1.begin();
    mode = Mode::Recording; print_mode();
    recByteSaved = 0L;
  } else {
    Serial.println("Couldn't open file to record!");
  }
}


void continueRecording() {
  #if defined(INSTRUMENT_SD_WRITE)
    uint32_t started = micros();
  #endif // defined(INSTRUMENT_SD_WRITE)
  #define NBLOX 16  
  // Check if there is data in the queue
  if (queue1.available() >= NBLOX) {
    byte buffer[NBLOX*AUDIO_BLOCK_SAMPLES*sizeof(int16_t)];
    // Fetch 2 blocks from the audio library and copy
    // into a 512 byte buffer.  The Arduino SD library
    // is most efficient when full 512 byte sector size
    // writes are used.
    for (int i=0;i<NBLOX;i++)
    {
      memcpy(buffer+i*AUDIO_BLOCK_SAMPLES*sizeof(int16_t), queue1.readBuffer(), AUDIO_BLOCK_SAMPLES*sizeof(int16_t));
      queue1.freeBuffer();
    }
    // Write all 512 bytes to the SD card
    frec.write(buffer, sizeof buffer);
    recByteSaved += sizeof buffer;
  }
  
  #if defined(INSTRUMENT_SD_WRITE)
    started = micros() - started;
    if (started > worstSDwrite)
      worstSDwrite = started;

    if (millis() >= printNext)
    {
      Serial.printf("Worst write took %luus\n",worstSDwrite);
      worstSDwrite = 0;
      printNext = millis()+250;
    }
  #endif // defined(INSTRUMENT_SD_WRITE)
}

void stopRecording() {
  // Stop adding any new data to the queue
  queue1.end();
  // Flush all existing remaining data from the queue
  while (queue1.available() > 0) {
    // Save to open file
    frec.write((byte*)queue1.readBuffer(), AUDIO_BLOCK_SAMPLES*sizeof(int16_t));
    queue1.freeBuffer();
    recByteSaved += AUDIO_BLOCK_SAMPLES*sizeof(int16_t);
  }
  writeOutHeader();
  // Close the file
  frec.close();
  Serial.println("Closed file");
  mode = Mode::Ready; print_mode();
  setMTPdeviceChecks(true); // enable MTP device checks, recording is finished
}


void playAllRecordings() {
  for (uint16_t i=0; i<500; i++) {
    // Format the counter as a five-digit number with leading zeroes, followed by file extension
    snprintf(filename, 11, " %05d.wav", i);
    // check, if file with index i exists
    if (SD.exists(filename)) {
      waveform1.begin(beep_volume_low, 440, WAVEFORM_SINE);
      if (wait(1000)) {
        waveform1.amplitude(0);
        
        Serial.println(filename);
        playWav1.play(filename);
        while (!playWav1.isStopped()) { // this works for playWav
          hook.update();
          char key = keypad.getKey();

          if(hook.risingEdge() || key == '7' || key == '1' || key == '#') {
            playWav1.stop();

            if (hook.risingEdge()) {
              mode = Mode::Ready; print_mode();
              return;
            } else if (key == '7') {
              SD.remove(filename);
              Serial.print(filename);
              Serial.println(" deleted");
            } else if (key == '1') {
              Serial.println("Play previous message");
              // Find previous message
              if (i != 0) {
                for (uint16_t j=i-1; j>=0; j--) {
                  snprintf(filename, 11, " %05d.wav", j);
                  if (SD.exists(filename)) {
                    Serial.println(j);
                    i = j - 1;
                    break;
                  }
                }
              }
            } else if (key == '#') {
              Serial.println("Play next message");
            }
          }
        }
      } else {
        waveform1.amplitude(0);
        mode = Mode::Ready; print_mode();
        return;
      }
    }
  }

  // All files have been played
  mode = Mode::Ready; print_mode();  
  end_Beep();
}


void playLastRecording() {
  for (uint16_t i=500; i>=0; i--) {
    // Format the counter as a five-digit number with leading zeroes, followed by file extension
    snprintf(filename, 11, " %05d.wav", i);
    // check, if file with index i exists
    if (SD.exists(filename)) {
      break;
    } else if (i == 0) {
      end_Beep();
      mode = Mode::Ready; print_mode();
      return;
    }
  }

  // Play file with index idx == last recorded file
  Serial.println(filename);
  playWav1.play(filename);
  while (!playWav1.isStopped()) { // this works for playWav
    hook.update();
    char key = keypad.getKey();

    if(hook.risingEdge() || key == '7') {
      playWav1.stop();

      if (key == '7') {
        SD.remove(filename);
        Serial.print(filename);
        Serial.println(" deleted");
        end_Beep();
      }
      
      mode = Mode::Ready; print_mode();
      return;
    }
  }
  // file has been played
  mode = Mode::Ready; print_mode();  
  end_Beep();
}

// Non-blocking delay, which pauses execution of main program logic,
// but while still listening for input 
// Returns false on hook interrupt and true, if no interrupt.
boolean wait(unsigned int milliseconds) {
  elapsedMillis msec=0;

  while (msec <= milliseconds) {
    hook.update();
    if (hook.fallingEdge()) {
      Serial.println("Hook Press");
      return false;
    }
    if (hook.risingEdge()) {
      Serial.println("Hook Release"); 
    }
  }
  return true;
}


void writeOutHeader() { // update WAV header with final filesize/datasize

//  NumSamples = (recByteSaved*8)/bitsPerSample/numChannels;
//  Subchunk2Size = NumSamples*numChannels*bitsPerSample/8; // number of samples x number of channels x number of bytes per sample
  Subchunk2Size = recByteSaved - 42; // because we didn't make space for the header to start with! Lose 21 samples...
  ChunkSize = Subchunk2Size + 34; // was 36;
  frec.seek(0);
  frec.write("RIFF");
  byte1 = ChunkSize & 0xff;
  byte2 = (ChunkSize >> 8) & 0xff;
  byte3 = (ChunkSize >> 16) & 0xff;
  byte4 = (ChunkSize >> 24) & 0xff;  
  frec.write(byte1);  frec.write(byte2);  frec.write(byte3);  frec.write(byte4);
  frec.write("WAVE");
  frec.write("fmt ");
  byte1 = Subchunk1Size & 0xff;
  byte2 = (Subchunk1Size >> 8) & 0xff;
  byte3 = (Subchunk1Size >> 16) & 0xff;
  byte4 = (Subchunk1Size >> 24) & 0xff;  
  frec.write(byte1);  frec.write(byte2);  frec.write(byte3);  frec.write(byte4);
  byte1 = AudioFormat & 0xff;
  byte2 = (AudioFormat >> 8) & 0xff;
  frec.write(byte1);  frec.write(byte2); 
  byte1 = numChannels & 0xff;
  byte2 = (numChannels >> 8) & 0xff;
  frec.write(byte1);  frec.write(byte2); 
  byte1 = sampleRate & 0xff;
  byte2 = (sampleRate >> 8) & 0xff;
  byte3 = (sampleRate >> 16) & 0xff;
  byte4 = (sampleRate >> 24) & 0xff;  
  frec.write(byte1);  frec.write(byte2);  frec.write(byte3);  frec.write(byte4);
  byte1 = byteRate & 0xff;
  byte2 = (byteRate >> 8) & 0xff;
  byte3 = (byteRate >> 16) & 0xff;
  byte4 = (byteRate >> 24) & 0xff;  
  frec.write(byte1);  frec.write(byte2);  frec.write(byte3);  frec.write(byte4);
  byte1 = blockAlign & 0xff;
  byte2 = (blockAlign >> 8) & 0xff;
  frec.write(byte1);  frec.write(byte2); 
  byte1 = bitsPerSample & 0xff;
  byte2 = (bitsPerSample >> 8) & 0xff;
  frec.write(byte1);  frec.write(byte2); 
  frec.write("data");
  byte1 = Subchunk2Size & 0xff;
  byte2 = (Subchunk2Size >> 8) & 0xff;
  byte3 = (Subchunk2Size >> 16) & 0xff;
  byte4 = (Subchunk2Size >> 24) & 0xff;  
  frec.write(byte1);  frec.write(byte2);  frec.write(byte3);  frec.write(byte4);
  frec.close();
  Serial.println("header written"); 
  Serial.print("Subchunk2: "); 
  Serial.println(Subchunk2Size); 
}

void end_Beep(int beeps, float volume) {
  waveform1.frequency(523.25);
  for(int i=0; i<beeps; i++) {
    waveform1.amplitude(volume);
    wait(250);
    waveform1.amplitude(0);
    wait(250);
  }
}

void print_mode(void) { // only for debugging
  Serial.print("Mode switched to: ");
  // Initialising, Ready, Prompting, Recording
  if(mode == Mode::Ready)           Serial.println(" Ready");
  else if(mode == Mode::Prompting)  Serial.println(" Prompting");
  else if(mode == Mode::Recording)  Serial.println(" Recording");
  else if(mode == Mode::Initialising)  Serial.println(" Initialising");
  else Serial.println(" Undefined");
}