#include "stdint.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"
#include "string.h"
#include <cstring>


typedef void (*RPCFunctionPtr)(uint8_t*, size_t, uint8_t*&, size_t&);
typedef void (*ResponseCallback)(uint32_t id, uint8_t *data, size_t length, bool success);

struct FunctionEntry {
    const char *name;
    RPCFunctionPtr function;
};

// Структура для ответов
struct Response {
    uint32_t id;
    uint8_t *data;
    size_t length;
    bool isError;
};

// Структура для ожидающих запросов
struct PendingRequest {
    uint32_t id;
    ResponseCallback callback;
};

#pragma pack(push, 1)
struct RPC_Packet {
    uint8_t sync1;      // 0xFA
    uint16_t length;    // Длина данных
    uint8_t headerCRC;  // CRC заголовка
    uint8_t sync2;      // 0xFB
    uint8_t data[0];    // Указатель на данные
};

struct Message {
    uint8_t Type;		     // Тип сообщения
    uint32_t ID;		     // Id сообщения
    char NameFunction[32];   // Фиксированный буфер для имени функции
    uint8_t separator;       // Разделитель
    uint8_t arguments[];     // Аргументы для удаленной функции
};
#pragma pack(pop)

class RPC {
	private:
		uint32_t nextId = 1;				 // Id сообщения при запросе

		FunctionEntry functions[10];         // Список зарегистрированых функций
		uint8_t functionCount = 0;			 // Количество зарегистрированных функций

		PendingRequest pendingRequests[10];  // Ожидающие запросы с колбэками
		uint8_t pendingCount = 0;

		uint8_t RxBuffer[1024];				 // Буфер для приема байтов данных
		uint16_t index = 0;					 // Размер данных в буфере

		enum MessageType {
			MSG_REQUEST  = 0x01,    // Запрос
			MSG_RESPONSE = 0x02,    // Ответ
			MSG_STREAM   = 0x03,    // Поток данных
			MSG_ERROR    = 0x04     // Ошибка
		};

		enum StepRec {
			WAIT_SYNC,		  // Ожидание начала приема
			WAIT_LEN_L,		  // Ожидание младшего байта длинны
			WAIT_LEN_H,		  // Ожидание старшего байта длинны
			WAIT_DATA	      // Ожидание данных
		};

		uint8_t state = WAIT_SYNC;

		uint16_t packet_length = 0;

		QueueHandle_t packetQueue;                   // Очередь для принятых пакетов
		QueueHandle_t responseQueue;                 // Очередь для ответов
		TaskHandle_t processingTaskHandle;

		TaskHandle_t responseTaskHandle;

		static void ProcessingTaskWrapper(void *params) {
			RPC* rpc = static_cast<RPC*>(params);
			rpc->ProcessingTask();
		}

		static void ResponseTaskWrapper(void *params) {
			RPC* rpc = static_cast<RPC*>(params);
			rpc->ResponseTask();
		}

		void ProcessingTask();   			      // Задача по изъятию принятых пакетов из очереди
		void HandlePacket(RPC_Packet *packet);	  // Проверка сообщения и передача в обработчик
		void ProcessMessage(uint8_t type, uint32_t id, const char *functionName, uint8_t *args, size_t argsLength);    // Обработчик принятых сообщений

		void ResponseTask();	// Задача по работе с принятыми ответами на запросы

		void ProcessIncomingResponse(uint32_t id, uint8_t *data, size_t length, bool isError);   // Обрабокта входящий ответов на запросы

		void SendResponse(uint8_t type, uint32_t id, bool success, uint8_t *responseData, size_t responseLength);  // Отправка ответа на запрос
		void SendPacket(uint8_t *buffer, uint16_t PacketLength) {/*отправка в uart*/};

		uint8_t CalculateCRC8(uint8_t crc, uint8_t data);        // Вычисление crc8
		bool CheckCRC(uint8_t *buffer, uint16_t totalLength);    // Проверить контрольную сумму пакета

		void FreeResponse(Response& response);  // Освободить память
		bool CallFunction(const char *name, uint8_t* args, size_t argsLength, uint8_t *&response, size_t &responseLength);   // Вызов зарегистрированной функции

	public:
		RPC();
		~RPC();

		void Start();   // Запуск задач при старте

		void RegisterFunction(const char *name, RPCFunctionPtr function);   // Регистрация функций для удаленного вызова

		void AddBuffer(uint8_t data);   // Складывание принятых байт в буфер, вызывать из прерывания

		bool SendRequest(const char *functionName, uint8_t *args, size_t argsLength, ResponseCallback callback = nullptr);   // Отправить запрос на устройство

};
