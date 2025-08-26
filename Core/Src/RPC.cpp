#include "RPC.h"

RPC::RPC() {
    packetQueue = xQueueCreate(10, sizeof(RPC_Packet*));
    responseQueue = xQueueCreate(10, sizeof(Response));

    processingTaskHandle = nullptr;
    responseTaskHandle = nullptr;

    functionCount = 0;
    pendingCount = 0;
    index = 0;
    packet_length = 0;

    state = WAIT_SYNC;

    for (int i = 0; i < 10; i++) {
        pendingRequests[i].id = 0;
        pendingRequests[i].callback = nullptr;
    }
}

RPC::~RPC() {
    if (processingTaskHandle) vTaskDelete(processingTaskHandle);

    if (responseTaskHandle) vTaskDelete(responseTaskHandle);

    vQueueDelete(packetQueue);
    vQueueDelete(responseQueue);
}

void RPC::Start() {

    // Задача для обработки входящих пакетов
    xTaskCreate(
        ProcessingTaskWrapper,
        "RPC_Processor",
        2 * 1024,
        this,
        tskIDLE_PRIORITY + 2,
        &processingTaskHandle
    );

    // Задача для обработки ответов
    xTaskCreate(
        ResponseTaskWrapper,
        "RPC_Response",
        2 * 1024,
        this,
        tskIDLE_PRIORITY + 1,
        &responseTaskHandle
    );
}

void RPC::ProcessingTask() {
    RPC_Packet* packet = nullptr;

    while(1) {
        if (xQueueReceive(packetQueue, &packet, portMAX_DELAY) == pdPASS) {
            HandlePacket(packet);
            vPortFree(packet);
        }
    }
}

// Разбор ответов на запросы
void RPC::ResponseTask() {
    Response response;

    while(1) {
        if (xQueueReceive(responseQueue, &response, portMAX_DELAY) == pdPASS) {
            for (int i = 0; i < pendingCount; i++) {

                if (pendingRequests[i].id == response.id) {		// Ищем ожидающие запросы ожидающие ответа

                    if (pendingRequests[i].callback) {
                    	pendingRequests[i].callback(response.id, response.data, response.length, !response.isError);   // Вызов колбека для обработки ответа
                    	FreeResponse(response);
                    	break;
                    }

                    for (int j = i; j < pendingCount - 1; j++) {
                        pendingRequests[j] = pendingRequests[j + 1];
                    }

                    pendingCount--;
                    break;
                }
            }

            FreeResponse(response);
        }
    }
}

void RPC::HandlePacket(RPC_Packet *packet) {
    uint8_t* payload = packet->data;
    uint16_t payloadLength = packet->length;

    Message *msg = reinterpret_cast<Message*>(payload);   // Приведение к типу Message

    if (msg->separator != 0) return;					  // Если нет разделительного 0 то выходим

    uint8_t *args = msg->arguments;
    size_t argsLength = payloadLength - offsetof(Message, arguments);

    ProcessMessage(msg->Type, msg->ID, msg->NameFunction, args, argsLength);
}

bool RPC::SendRequest(const char *functionName, uint8_t *args, size_t argsLength, ResponseCallback callback) {

    uint32_t outId = nextId++;

    if (callback && pendingCount < 10) {
        pendingRequests[pendingCount].id = outId;
        pendingRequests[pendingCount].callback = callback;
        pendingCount++;
    }

    // Формируем сообщение запроса
    size_t nameLen = strlen(functionName) + 1;
    size_t messageLength = offsetof(Message, arguments) + nameLen + argsLength;
    size_t totalPacketLength = sizeof(RPC_Packet) + messageLength + 2;

    uint8_t* packetBuffer = (uint8_t*)pvPortMalloc(totalPacketLength);
    RPC_Packet* packet = (RPC_Packet*)packetBuffer;

    // Формирование заголовка
    packet->sync1 = 0xFA;
    packet->length = messageLength;
    packet->headerCRC = CalculateCRC8(0, packet->sync1);
    packet->headerCRC = CalculateCRC8(packet->headerCRC, packet->length & 0xFF);
    packet->headerCRC = CalculateCRC8(packet->headerCRC, (packet->length >> 8) & 0xFF);
    packet->sync2 = 0xFB;

    // Формирование сообщения
    Message *msg = (Message*)packet->data;
    msg->Type = MSG_REQUEST;
    msg->ID = outId;
    msg->separator = 0;
    strncpy(msg->NameFunction, functionName, sizeof(msg->NameFunction) - 1);
    msg->NameFunction[sizeof(msg->NameFunction) - 1] = '\0';

    if (argsLength > 0) memcpy(msg->arguments, args, argsLength);

    // Рассчитываем CRC
    uint8_t dataCRC = 0;
    for (size_t i = 0; i < messageLength; i++) {
        dataCRC = CalculateCRC8(dataCRC, packet->data[i]);
    }

    packetBuffer[sizeof(RPC_Packet) + messageLength] = dataCRC;
    packetBuffer[sizeof(RPC_Packet) + messageLength + 1] = 0xFE;

    // Отправляем пакет
    SendPacket(packetBuffer, totalPacketLength);

    vPortFree(packetBuffer);
    return true;
}

void RPC::ProcessIncomingResponse(uint32_t id, uint8_t *data, size_t length, bool isError) {
    Response response;
    response.id = id;
    response.data = (uint8_t*)pvPortMalloc(length);
    response.length = length;
    response.isError = isError;

    // По идее надо проверять есть ли выделение памяти
    memcpy(response.data, data, length);

    xQueueSend(responseQueue, &response, 0);
}


void RPC::FreeResponse(Response &response) {
    if (response.data != nullptr) {
        vPortFree(response.data);
        response.data = nullptr;
        response.length = 0;
    }
}

void RPC::ProcessMessage(uint8_t type, uint32_t id, const char *functionName, uint8_t *args, size_t argsLength) {
    uint8_t *response = nullptr;
    size_t responseLength = 0;
    bool success = false;

    switch(type) {

        case MSG_REQUEST:
            success = CallFunction(functionName, args, argsLength, response, responseLength);  // Если был запрос, то выполняем функцию и отдаем ответ
            SendResponse(type, id, success, response, responseLength);
            break;

        case MSG_RESPONSE:
            ProcessIncomingResponse(id, args, argsLength, false);	// Разбор ответа, положить в очередь приема ответов
            break;

        case MSG_ERROR:
            ProcessIncomingResponse(id, args, argsLength, true);    // Разбор ошибки, положить в очередь приема ответов
            break;

        case MSG_STREAM:
        	/*
        	 * Как я понял это получение данных частями, когда длинна сообщеня превышает размер пакета
        	 */
            break;
    }

    if (response != nullptr) vPortFree(response);
}

// Формирование сообщения ответа и отдача в физический уровень
void RPC::SendResponse(uint8_t type, uint32_t id, bool success, uint8_t *responseData, size_t responseLength) {
    size_t messageLength = offsetof(Message, arguments) + responseLength;
    size_t totalPacketLength = sizeof(RPC_Packet) + messageLength + 2;

    uint8_t* packetBuffer = (uint8_t*)pvPortMalloc(totalPacketLength);
    RPC_Packet* packet = (RPC_Packet*)packetBuffer;

    packet->sync1 = 0xFA;
    packet->length = messageLength;
    packet->headerCRC = CalculateCRC8(0, packet->sync1);
    packet->headerCRC = CalculateCRC8(packet->headerCRC, packet->length & 0xFF);
    packet->headerCRC = CalculateCRC8(packet->headerCRC, (packet->length >> 8) & 0xFF);
    packet->sync2 = 0xFB;

    Message* msg = (Message*)packet->data;
    msg->Type = success ? MSG_RESPONSE : MSG_ERROR;
    msg->ID = id;
    msg->separator = 0;

    if (responseData != nullptr && responseLength > 0) memcpy(msg->arguments, responseData, responseLength);

    uint8_t dataCRC = 0;
    for (size_t i = 0; i < messageLength; i++) {
        dataCRC = CalculateCRC8(dataCRC, packet->data[i]);
    }

    packetBuffer[sizeof(RPC_Packet) + messageLength] = dataCRC;
    packetBuffer[sizeof(RPC_Packet) + messageLength + 1] = 0xFE;

    SendPacket(packetBuffer, totalPacketLength);
    vPortFree(packetBuffer);
}

void RPC::RegisterFunction(const char *name, RPCFunctionPtr function) {
    if (functionCount < 10) {
        functions[functionCount].name = name;
        functions[functionCount].function = function;
        functionCount++;
    }
}

bool RPC::CallFunction(const char *name, uint8_t *args, size_t argsLength, uint8_t* &response, size_t &responseLength) {
    for (int i = 0; i < functionCount; i++) {
        if (strcmp(functions[i].name, name) == 0) {
            functions[i].function(args, argsLength, response, responseLength);
            return true;
        }
    }
    return false;
}

void RPC::AddBuffer(uint8_t data) {

    switch(state) {

        case WAIT_SYNC:
            if (data == 0xFA) {
                index = 0;
                RxBuffer[index++] = data;
                state = WAIT_LEN_L;
            }
            break;

        case WAIT_LEN_L:
            RxBuffer[index++] = data;
            packet_length = data;
            state = WAIT_LEN_H;
            break;

        case WAIT_LEN_H:
            RxBuffer[index++] = data;
            packet_length |= (data << 8);
            state = WAIT_DATA;
            break;

        case WAIT_DATA:
            RxBuffer[index++] = data;
            if (index >= packet_length + 5) {
                if (RxBuffer[index - 1] == 0xFE) {
                    if (!CheckCRC(RxBuffer, index)) return;

                    RPC_Packet *packetCopy = (RPC_Packet*)pvPortMalloc(index);
                    memcpy(packetCopy, RxBuffer, index);
                    xQueueSend(packetQueue, &packetCopy, 0);
                }
                state = WAIT_SYNC;
                index = 0;
            }
            break;
    }
}


bool RPC::CheckCRC(uint8_t *buffer, uint16_t totalLength) {
    if (buffer[totalLength - 1] != 0xFE) return false;

    RPC_Packet *packet = (RPC_Packet*)buffer;
    uint8_t headerCRC = CalculateCRC8(0, packet->sync1);
    headerCRC = CalculateCRC8(headerCRC, packet->length & 0xFF);
    headerCRC = CalculateCRC8(headerCRC, (packet->length >> 8) & 0xFF);
    if (headerCRC != packet->headerCRC) return false;

    uint8_t dataCRC = 0;
    for (int i = 0; i < packet->length; i++) {
        dataCRC = CalculateCRC8(dataCRC, packet->data[i]);
    }

    return dataCRC == buffer[totalLength - 2];
}

uint8_t RPC::CalculateCRC8(uint8_t crc, uint8_t data) {
    crc ^= data;
    for (int i = 0; i < 8; i++) {
        if (crc & 0x80) crc = (crc << 1) ^ 0x07;
        else 	        crc = (crc << 1);
    }
    return crc;
}
