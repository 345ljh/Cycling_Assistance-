#include "esp32.h"

/* 协议内容 帧头3byte
固定开头 0x02 1byte
消息内容 1byte
数据长度 1byte
数据 nbyte
固定结尾 0x04 1byte

发送当前gps定位 0x11 16byte
经纬度 16byte

(v1/v2)发送目的地名称  0x12 80byte
目的地名称uint16 30*2=60byte
城市名称uint16 20byte

(v2)发送搜索地名称  0x13 80byte
同0x12

(v2)搜索后发送选定目的地 0x14 1byte
目的地0~4 1byte

接收测试信息 0x90 16byte

成功规划路径 0x91 31byte
路径长度(m) uint32 4byte
用时(s) uint16 2byte
路径点数量 uint8 1byte
路径中心 2double 16byte
路径正方形1/2边长 double 8byte

路径点 0x92 82byte 一次最多发5个有效点
包序号uint8 1byte
路径点经纬度double 80byte

规划失败 0x93 1byte
错误信息uint8 1byte

(v2)搜索到目的地信息 0x94 123byte
搜索地点数 1byte
当前序号 1byte
长度 1byte
编码为(u8,u8,u16)的地名 <=4byte*30=120byte

完整数据<=128byte
*/

ESP32 *ESP32_Init(UART_HandleTypeDef *huart){
    ESP32 *obj = (ESP32 *)malloc(sizeof(ESP32));
    memset(obj, 0, sizeof(ESP32));
    obj->rx_data_temp.routepoint.routes = (Point*)malloc(sizeof(Point));
    obj->huart = huart;
    HAL_DMA_DeInit(huart->hdmatx);
    HAL_DMA_DeInit(huart->hdmarx);
    HAL_DMA_Init(huart->hdmatx);
    HAL_DMA_Init(huart->hdmarx);
    HAL_UART_DMAStop(huart);
    // 使能串口空闲中断
    __HAL_UART_ENABLE_IT(huart, UART_IT_IDLE);
    HAL_UART_Receive_DMA(huart, obj->rxbuf, ESP_BUFFER_LEN);

    return obj;
}

void ESP32_SendOriginPosition(ESP32* obj, Point origin){
    uint8_t tx_buf[SEND_ORIGIN_POSITION_DATALEN + 4] = {0x02, 0x11, SEND_ORIGIN_POSITION_DATALEN};
    memcpy(tx_buf + 3, (uint8_t*)&origin, 16);
    tx_buf[SEND_ORIGIN_POSITION_DATALEN + 3] = 0x04;
    HAL_UART_Transmit(obj->huart, tx_buf, SEND_ORIGIN_POSITION_DATALEN + 4, 1000);
}

void ESP32_SendDestinationName(ESP32* obj, uint16_t* name, uint16_t* city){
    uint8_t tx_buf[SEND_DESTINATION_NAME_DATALEN + 4] = {0x02, 0x12, SEND_DESTINATION_NAME_DATALEN};
    memcpy(tx_buf + 3, (uint8_t*)name, 60);
    memcpy(tx_buf + 63, (uint8_t*)city, 20);
    tx_buf[SEND_DESTINATION_NAME_DATALEN + 3] = 0x04;
    HAL_UART_Transmit(obj->huart, tx_buf, SEND_DESTINATION_NAME_DATALEN + 4, 1000);
}

void ESP32_SendSearchName(ESP32* obj, uint16_t* name, uint16_t* city){
    uint8_t tx_buf[SEND_DESTINATION_NAME_DATALEN + 4] = {0x02, 0x13, SEND_DESTINATION_NAME_DATALEN};
    memcpy(tx_buf + 3, (uint8_t*)name, 60);
    memcpy(tx_buf + 63, (uint8_t*)city, 20);
    tx_buf[SEND_DESTINATION_NAME_DATALEN + 3] = 0x04;
    HAL_UART_Transmit(obj->huart, tx_buf, SEND_DESTINATION_NAME_DATALEN + 4, 1000);
}

void ESP32_SendSearchSelection(ESP32* obj, uint8_t sel){
    uint8_t tx_buf[SEND_SEARCH_SELECTION_DATALEN + 4] = {0x02, 0x14, SEND_SEARCH_SELECTION_DATALEN};
    memcpy(tx_buf + 3, &sel, 1);
    tx_buf[SEND_SEARCH_SELECTION_DATALEN + 3] = 0x04;
    HAL_UART_Transmit(obj->huart, tx_buf, SEND_SEARCH_SELECTION_DATALEN + 4, 1000);
}

void ESP32_RxCallback(ESP32 *obj){
    __HAL_UART_CLEAR_IDLEFLAG(obj->huart);  // 清除中断标志位
    HAL_UART_DMAStop(obj->huart);
    ESP32_Solve(obj);
    memset(obj->rxbuf, 0, ESP_BUFFER_LEN);
    HAL_UART_Receive_DMA(obj->huart, obj->rxbuf, ESP_BUFFER_LEN);
}

void ESP32_Solve(ESP32 *obj){
    if(obj->rxbuf[0] != 0x02) return;
    if(obj->rxbuf[1] == 0x90){
        memcpy((uint8_t*)&obj->rx_data_temp.rxtest, obj->rxbuf + 3, 16);
    } else if(obj->rxbuf[1] == 0x91){
        printf_log("received 0x91\n");
        memcpy((uint8_t*)&obj->rx_data_temp.routepoint.distance, obj->rxbuf + 3, 4);
        memcpy((uint8_t*)&obj->rx_data_temp.routepoint.duration, obj->rxbuf + 7, 2);
        memcpy((uint8_t*)&obj->rx_data_temp.routepoint.len, obj->rxbuf + 9, 1);
        memcpy((uint8_t*)&obj->rx_data_temp.routepoint.center, obj->rxbuf + 10, 16);
        memcpy((uint8_t*)&obj->rx_data_temp.routepoint.halfside, obj->rxbuf + 26, 8);

        obj->rx_data_temp.routepoint.halfside *= 1.05;

        free(obj->rx_data_temp.routepoint.routes);
        obj->rx_data_temp.routepoint.routes = (Point*)calloc(sizeof(Point), obj->rx_data_temp.routepoint.len);
        memset(obj->rx_data_temp.routepoint.routes, 0, sizeof(Point) * obj->rx_data_temp.routepoint.len);
    } else if(obj->rxbuf[1] == 0x92){
        printf_log("received 0x92\n");
        for(int j = 0; j < 5; j++){
            if(5 * obj->rxbuf[3] + j < obj->rx_data_temp.routepoint.len){
                memcpy((uint8_t*)&(obj->rx_data_temp.routepoint.routes[5 * obj->rxbuf[3] + j]), obj->rxbuf + 4 + 16 * j, 16);
            }
        }
        signal_flag = 0x3E;
    } else if(obj->rxbuf[1] == 0x93){
        printf_log("received 0x93\n");
        signal_flag = obj->rxbuf[3] + 0xF0;
    } else if(obj->rxbuf[1] == 0x94){
        printf_log("received 0x94\n");
        obj->rx_data_temp.searchname.selection_cnt = obj->rxbuf[3];
        obj->rx_data_temp.searchname.selection[obj->rxbuf[4]].len = obj->rxbuf[5];
        memcpy((uint8_t*)obj->rx_data_temp.searchname.selection[obj->rxbuf[4]].chs, obj->rxbuf + 6, obj->rxbuf[5] * 4);
        signal_flag = 0x2F;
    }
}