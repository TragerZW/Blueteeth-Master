#include "Blueteeth-Master.h"

int scanTime = 5; //In seconds
char input_buffer[MAX_BUFFER_SIZE];
BLEScan* pBLEScan;
SemaphoreHandle_t uartMutex;
TaskHandle_t terminalInputTaskHandle;
TaskHandle_t ringTokenWatchdogTaskHandle;
TaskHandle_t packetReceptionTaskHandle;
TaskHandle_t dataStreamPackagerTaskHandle;

void terminalInputTask ( void * );
void ringTokenWatchdogTask( void * );
void packetReceptionTask( void * );
void dataStreamPackagerTask( void * );
void dataStreamMonitorTask( void * );

terminalParameters_t terminalParameters;
int discoveryIdx;

BluetoothA2DPSink a2dpSink;

BlueteethMasterStack internalNetworkStack(10, &packetReceptionTaskHandle, &Serial2, &Serial1); //Serial1 = Data Plane, Serial2 = Control Plane
BlueteethBaseStack * internalNetworkStackPtr = &internalNetworkStack; //Need pointer for run-time polymorphism

volatile bool streamActive;

/*  Callback for when data is received from A2DP BT stream
*   
*   @data - Pointer to an array with the individual bytes received.
*   @length - The number of bytes received.
*/ 
void a2dpSinkDataReceived(const uint8_t *data, uint32_t length){
  // Serial.print("BLUETOOTH DATA RECEIVED!");
  
  internalNetworkStack.recordDataBufferAccessTime();

  for (int i = 0; i < length; i++){
    internalNetworkStack.dataBuffer.push_back(data[i]);
  }

  if (streamActive == false){
    vTaskResume(dataStreamPackagerTaskHandle);
    streamActive = true;
  }
}

void read_data_stream(const uint8_t *data, uint32_t length) {
    // process all data
    int16_t *values = (int16_t*) data;
    for (int j=0; j<length/2; j+=2){
      // print the 2 channel values
      Serial.print(values[j]);
      Serial.print(",");
      Serial.println(values[j+1]);
    }
}

void setup() {
  
  //Start Serial comms
  Serial.begin(115200);
  uartMutex = xSemaphoreCreateMutex(); //mutex for UART

  internalNetworkStack.begin();
  
  //Setup Peripherals
  // pBLEScan = bleScanSetup();
  
  xTaskCreate(dataStreamPackagerTask, // Task function
  "DATA STREAM PACKAGER", // Task name
  4096, // Stack depth 
  NULL, 
  24, // Priority
  &dataStreamPackagerTaskHandle); // Task handler
  
  //Create tasks
  xTaskCreate(terminalInputTask, // Task function
  "UART TERMINAL INPUT", // Task name
  4096, // Stack depth
  NULL, 
  1, // Priority
  &terminalInputTaskHandle); // Task handler
  
  xTaskCreate(ringTokenWatchdogTask, // Task function
  "RING TOKEN WATCHDOG", // Task name
  4096, // Stack depth 
  NULL, 
  1, // Priority
  &ringTokenWatchdogTaskHandle); // Task handler

  xTaskCreate(packetReceptionTask, // Task function
  "PACKET RECEPTION HANDLER", // Task name
  4096, // Stack depth 
  NULL, 
  1, // Priority
  &packetReceptionTaskHandle); // Task handler

  a2dpSink.set_stream_reader(a2dpSinkDataReceived);
  a2dpSink.set_auto_reconnect(false);
  a2dpSink.start("Blueteeth Sink"); //Begin advertising
}

void loop() {
}


/*  Checks to see if the ring token is still in the network. If it isn't detected after some period, generates a new token.
*
*/  
void ringTokenWatchdogTask(void * params) {
  while (1){
    vTaskDelay(RING_TOKEN_GENERATION_DELAY_MS);
    if (internalNetworkStack.getTokenRxFlag() == false){
      Serial.print("Generating a new token.\n\r"); //DEBUG STATEMENT
      // internalNetworkStack.tokenReceived();
      internalNetworkStack.generateNewToken();
    }
    internalNetworkStack.resetTokenRxFlag(); 
  }
}

void dataStreamPackagerTask(void * params) {

  uint8_t tmp[MAX_DATA_PLANE_PAYLOAD_SIZE / PAYLOAD_SIZE * FRAME_SIZE]; //temporary storage
  size_t dataLen;
  size_t frameLen;

  while (1){

    if (internalNetworkStack.dataBuffer.size() < 512) { 
      xSemaphoreGive(internalNetworkStack.dataBufferMutex); //give away mutex before suspending
      streamActive = false;
      vTaskSuspend(NULL);
      xSemaphoreTake(internalNetworkStack.dataBufferMutex, portMAX_DELAY); //take it back after coming out of suspension
    }

    dataLen = min((internalNetworkStack.dataBuffer.size() / PAYLOAD_SIZE) * PAYLOAD_SIZE, (size_t) MAX_DATA_PLANE_PAYLOAD_SIZE); 
    frameLen = ceil( (double) dataLen / PAYLOAD_SIZE * FRAME_SIZE);

    packDataStream(tmp, dataLen, internalNetworkStack.dataBuffer);
  
    internalNetworkStack.streamData(tmp, frameLen); 

  }
}

/*  Gets individual bytes of a 32 bit integer
*   
*   @integer - the integer being analyzed
*   @byteArray - array containing 4 bytes corresponding to a 32 bit integer
*   @return - the resulting integer
*/  
inline void int2Bytes(uint32_t integer, uint8_t * byteArray){
  for (int offset = 0; offset < 32; offset += 8){
    byteArray[offset/8] = integer >> offset; //assignment will truncate so only first 8 bits are assigned
  }
}

/*  Unpacks byte array into a 32 bit integer
*   
*   @byteArray - array containing 4 bytes corresponding to a 32 bit integer
*   @return - the resulting integer
*/  
inline uint32_t bytes2Int(uint8_t * byteArray){
  uint32_t integer = 0;
  for (int offset = 0; offset < 32; offset += 8){
    integer += byteArray[offset/8] << offset; 
  }
  return integer;
}

/*  Task that runs when a new Blueteeth packet is received. 
*
*/  
void packetReceptionTask (void * pvParams){
  while(1){
    
    vTaskSuspend(packetReceptionTaskHandle);
    BlueteethPacket packetReceived = internalNetworkStack.getPacket();

    switch(packetReceived.type){
      
      case PING:
        Serial.print("Ping packet type received.\n\r"); //DEBUG STATEMENT
        Serial.printf("Response from address %s\n\r", packetReceived.payload);
        break;
      
      case STREAM_RESULTS:
        if (bytes2Int(packetReceived.payload + 4) > 10000){
          Serial.print("Data stream failed\n\r");
        }
        else {
          Serial.printf("Stream results from ADDR%d: Checksum = %d, Time = %d\n\r", packetReceived.srcAddr, bytes2Int(packetReceived.payload), bytes2Int(packetReceived.payload + 4));
        }
        break;

      default:
        // Sometimes read noise on the line
        // Serial.print("Unknown packet type received.\n\r"); //DEBUG STATEMENT
        break;
    }

  }
}

/*  Prints all characters in a character buffer
*
*   @endPos - last buffer position that should be printed
*/ 
void inline printBuffer(int endPos){

  Serial.print("\0337"); //save cursor positon
  Serial.printf("\033[%dF", endPos + 1); //go up N + 1 lines
  for (int i = 0; i <= endPos; i++) {

    Serial.print("\033[2K"); //clear line
    
    switch(input_buffer[i]){
      case '\0':
        Serial.printf("Character %d = NULL\n\r", i);
        break;
      case '\n':
        Serial.printf("Character %d = NEWLINE\n\r", i);
        break;
      case 127:
        Serial.printf("Character %d = BACKSPACE\n\r", i);
        break;
      default:
        Serial.printf("Character %d = %c\n\r", i , input_buffer[i]);
    }
  }
  Serial.print("\0338"); //restore cursor position
}


#define DATA_STREAM_TIMEOUT (1000)
void dataStreamMonitorTask (void * pvParams){
  while(1){
    vTaskDelay(500);
    if ((internalNetworkStack.getTimeElapsedSinceLastDataBufferAccess() > DATA_STREAM_TIMEOUT)){
      // deque<uint8_t>().swap(internalNetworkStack.dataBuffer); 
      xSemaphoreTake(internalNetworkStack.dataBufferMutex, portMAX_DELAY);
      internalNetworkStack.dataBuffer.resize(0);
      xSemaphoreGive(internalNetworkStack.dataBufferMutex);
      // Serial.printf("Timeout achieved (new size is %d)\n\r", internalNetworkStack.dataBuffer.size());
    }
  }
}

/*  Take in user inputs and handle pre-defined commands.
*
*/
void terminalInputTask(void * params) {

  clear_buffer(input_buffer, sizeof(input_buffer));
  int buffer_pos = 0;
  BLEScanResults scanResults;
  const char * btTarget;
  
  while(1){

    vTaskDelay(100);

    xSemaphoreTake(uartMutex, portMAX_DELAY);

    while (Serial.available() && (buffer_pos < MAX_BUFFER_SIZE)){ //get number of bits on buffer
      
      input_buffer[buffer_pos] = Serial.read();

      //handle special chracters
      if (input_buffer[buffer_pos] == '\r') { //If an enter character is received
        
        input_buffer[buffer_pos] = '\0'; //Get rid of the carriage return
        Serial.print("\n\r");
        
        BlueteethPacket newPacket (false, internalNetworkStack.getAddress(), 254); //Need to declare prior to switch statement to avoid "crosses initilization" error.

        switch ( handle_input(input_buffer, terminalParameters) ){
          
          case CONNECT:
            newPacket.dstAddr = 1;
            newPacket.type = CONNECT;
            for (int address = 1; address <= 3; address++){
              newPacket.dstAddr = address;
              sprintf((char *) newPacket.payload, "Wireless Speaker");
              internalNetworkStack.queuePacket(1, newPacket);
            }
            break;
          
          case DROP:
            xSemaphoreTake(internalNetworkStack.dataBufferMutex, portMAX_DELAY);
            internalNetworkStack.dataBuffer.clear();
            xSemaphoreGive(internalNetworkStack.dataBufferMutex);
            // newPacket.type = DROP;
            // internalNetworkStack.queuePacket(1, newPacket);
            break;
          
          case DISCONNECT:
            newPacket.dstAddr = 1;
            newPacket.type = DISCONNECT;
            internalNetworkStack.queuePacket(1, newPacket);
            break;

          case PING:
            newPacket.type = PING;
            internalNetworkStack.queuePacket(1, newPacket);
            break;

          case INITIALIZAITON:
            newPacket.dstAddr = 255;
            newPacket.type = INITIALIZAITON;
            newPacket.payload[0] = 1;
            internalNetworkStack.queuePacket(1, newPacket);
            break;

          case STREAM: {
            
            internalNetworkStack.recordDataBufferAccessTime(); //This will stop the data stream monitor from resetting buffer
            xSemaphoreTake(internalNetworkStack.dataBufferMutex, portMAX_DELAY);
            internalNetworkStack.dataBuffer.resize(0);
            for (int i = 0; i < DATA_STREAM_TEST_SIZE; i++){
              internalNetworkStack.dataBuffer.push_back( (i % 255) + 1 );
            }
            xSemaphoreGive(internalNetworkStack.dataBufferMutex);
            uint32_t t = millis();

            uint8_t streamArray[255];
            for (int i = 0; i < 255; i++){
                streamArray[i]=i+1;
            }
            uint8_t cnt = 0;
            while (cnt < 158) {
              internalNetworkStack.streamData(streamArray, 255);
              cnt++;
            }

            t = millis() - t;
            Serial.printf("40 kByte transmission finished in %d milliseconds\n\r", t);
            
            BlueteethPacket streamRequest(false, internalNetworkStack.getAddress(), 254);
            streamRequest.type = STREAM;
            internalNetworkStack.queuePacket(true, streamRequest);
            break;
          }

          case TEST: {
            
            Serial.print("Attempting to stream sample audio data on the data plane\n\r");
            int cnt = 0;
            int cnt2;
            int streamChunk = 40000;
            while (cnt < sizeof(audioSamples)){
              internalNetworkStack.recordDataBufferAccessTime(); //This will stop the data buffer monitor from resetting buffer
              xSemaphoreTake(internalNetworkStack.dataBufferMutex, portMAX_DELAY);
              cnt2 = 0;
              while ( (cnt2 < streamChunk) && (cnt < sizeof(audioSamples))) {
                internalNetworkStack.dataBuffer.push_back(audioSamples[cnt++]);
                cnt2++;
              }
              xSemaphoreGive(internalNetworkStack.dataBufferMutex);
              if (streamActive == false){
                vTaskResume(dataStreamPackagerTaskHandle);
                streamActive = true;
              }
              while (streamActive){
               //Do nothing
              }
            }
            break;
          }
            
          default:
            break;
            //no action needed

        } //handle the input
        clear_buffer(input_buffer, sizeof(input_buffer));
        buffer_pos = -1; //return the buffer back to zero (incrimented after this statement)
        // Serial.printf("Buffer pos is %d", buffer_pos);
      }
      
      else if (input_buffer[buffer_pos] == 127){ //handle a backspace character
        Serial.printf("%c", 127); //print out backspace
        input_buffer[buffer_pos] = '\0'; //clear the backspace 
        if (buffer_pos > 0) input_buffer[--buffer_pos] = '\0'; //clear the previous buffer pos if there was another character in the buffer that wasn't a backspace
        buffer_pos--;
      }
      
      else Serial.printf("%c", input_buffer[buffer_pos]);

      buffer_pos++;
      
    }

    xSemaphoreGive(uartMutex);

  }
}