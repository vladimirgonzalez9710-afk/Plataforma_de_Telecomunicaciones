#ifndef UART_PROTOCOL_H
#define UART_PROTOCOL_H
#define MAX_DATA_LEN 64
#define MAX_RETRIES 5
#define TIMEOUT 2

int init_serial(const char *port);
int uart_start_connection(int fd, int total_packets);
int uart_end_connection(int fd, int success);

// Envío y recepción confiable
int uart_send_packet(int fd, int seq, const char *data);
int uart_receive_packet(int fd, char *data_out, int *seq_out);

// ACK/NACK
void uart_send_ack(int fd, int seq);
void uart_send_nack(int fd, int seq);

// Validación de datos
int uart_calculate_crc(const char *data);
int uart_check_crc(const char *data, int expected_crc);

// Manejo de errores
void uart_trigger_error(int fd);

#endif
