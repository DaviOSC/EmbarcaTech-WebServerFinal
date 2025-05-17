#include <stdio.h>  // Biblioteca padrão para entrada e saída
#include <string.h> // Biblioteca manipular strings
#include <stdlib.h> // funções para realizar várias operações, incluindo alocação de memória dinâmica (malloc)

#include "pico/stdlib.h"     // Biblioteca da Raspberry Pi Pico para funções padrão (GPIO, temporização, etc.)
#include "hardware/adc.h"    // Biblioteca da Raspberry Pi Pico para manipulação do conversor ADC
#include "pico/cyw43_arch.h" // Biblioteca para arquitetura Wi-Fi da Pico com CYW43
#include "hardware/pwm.h"    // Biblioteca da Raspberry Pi Pico para manipulação de PWM (modulação por largura de pulso)
#include "pico/bootrom.h"

#include "lwip/pbuf.h"  // Lightweight IP stack - manipulação de buffers de pacotes de rede
#include "lwip/tcp.h"   // Lightweight IP stack - fornece funções e estruturas para trabalhar com o protocolo TCP
#include "lwip/netif.h" // Lightweight IP stack - fornece funções e estruturas para trabalhar com interfaces de rede (netif)

// Credenciais WIFI - Tome cuidado se publicar no github!
#include "SSIDPASSWORD.h" // Arquivo de cabeçalho com as credenciais Wi-Fi
// #define WIFI_SSID ""
// #define WIFI_PASSWORD ""
//  Definição dos pinos dos LEDs
#define LED_PIN CYW43_WL_GPIO_LED_PIN // GPIO do CI CYW43
#define LED_BLUE_PIN 12               // GPIO12 - LED azul
#define LED_GREEN_PIN 11              // GPIO11 - LED verde
#define LED_RED_PIN 13                // GPIO13 - LED vermelho
#define BUZZER_PIN 10                 // GPIO10 - Buzzer
#define JOYSTICK_X_PIN 27
#define PIN_BUTTON_B 6
static int fan_speed = 0;     // Velocidade do ventilador (0-255)
static bool fan_on = false;   // Estado do ventilador
static bool auto_fan = false; // Indica se a velocidade foi ajustada via HTTP
float temp_c = 25.0f;
// Função de callback ao aceitar conexões TCP
static err_t tcp_server_accept(void *arg, struct tcp_pcb *newpcb, err_t err);

// Função de callback para processar requisições HTTP
static err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);

// Tratamento do request do usuário
void user_request(char **request);
static void gpio_irq_handler(uint gpio, uint32_t events);

// Função principal
int main()
{
    // Inicializa todos os tipos de bibliotecas stdio padrão presentes que estão ligados ao binário.
    stdio_init_all();
    adc_init();
    adc_gpio_init(JOYSTICK_X_PIN);
    gpio_init(PIN_BUTTON_B);
    gpio_set_dir(PIN_BUTTON_B, GPIO_IN);
    gpio_pull_up(PIN_BUTTON_B);
    gpio_set_irq_enabled_with_callback(PIN_BUTTON_B, GPIO_IRQ_EDGE_FALL, 1, &gpio_irq_handler);

    // Inicializa a arquitetura do cyw43
    while (cyw43_arch_init())
    {
        printf("Falha ao inicializar Wi-Fi\n");
        sleep_ms(100);
        return -1;
    }

    // Ativa o Wi-Fi no modo Station, de modo a que possam ser feitas ligações a outros pontos de acesso Wi-Fi.
    cyw43_arch_enable_sta_mode();

    // Conectar à rede WiFI - fazer um loop até que esteja conectado
    printf("Conectando ao Wi-Fi...\n");
    while (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 20000))
    {
        printf("Falha ao conectar ao Wi-Fi\n");
        sleep_ms(100);
        return -1;
    }
    printf("Conectado ao Wi-Fi\n");

    // Caso seja a interface de rede padrão - imprimir o IP do dispositivo.
    if (netif_default)
    {
        printf("IP do dispositivo: %s\n", ipaddr_ntoa(&netif_default->ip_addr));
    }

    // Configura o servidor TCP - cria novos PCBs TCP. É o primeiro passo para estabelecer uma conexão TCP.
    struct tcp_pcb *server = tcp_new();
    if (!server)
    {
        printf("Falha ao criar servidor TCP\n");
        return -1;
    }

    // vincula um PCB (Protocol Control Block) TCP a um endereço IP e porta específicos.
    if (tcp_bind(server, IP_ADDR_ANY, 80) != ERR_OK)
    {
        printf("Falha ao associar servidor TCP à porta 80\n");
        return -1;
    }

    // Coloca um PCB (Protocol Control Block) TCP em modo de escuta, permitindo que ele aceite conexões de entrada.
    server = tcp_listen(server);

    // Define uma função de callback para aceitar conexões TCP de entrada. É um passo importante na configuração de servidores TCP.
    tcp_accept(server, tcp_server_accept);
    printf("Servidor ouvindo na porta 80\n");

    // Inicializa o GPIO do LED Azul , que indica que o servidor está ativo.
    gpio_init(LED_BLUE_PIN);              // Inicializa o GPIO do LED azul
    gpio_set_dir(LED_BLUE_PIN, GPIO_OUT); // Define o GPIO do LED azul como saída
    gpio_put(LED_BLUE_PIN, true);         // Liga o LED azul

    // Inicializa e configura o PWM da GPIO do buzzer, pra que ele possa emitir sons.
    gpio_set_function(BUZZER_PIN, GPIO_FUNC_PWM);
    uint slice_num = pwm_gpio_to_slice_num(BUZZER_PIN);
    pwm_set_clkdiv(slice_num, 4.0);
    pwm_set_wrap(slice_num, 4095);
    pwm_set_enabled(slice_num, true);
    // Inicializa o conversor ADC
    adc_init();
    adc_select_input(1); // Seleciona o ADC do eixo X do joystick
    while (true)
    {
        cyw43_arch_poll(); // Necessário para manter o Wi-Fi ativo
        
        if (fan_on)
        {
            if (auto_fan)
            {
                
                uint16_t raw_value = adc_read();
                fan_speed = raw_value / 16; // Converte para 0-255
                temp_c = (fan_speed / 255.0f) * 50.0f;
            }

            // Ajusta a frequência e o volume do buzzer com base na velocidade
            uint slice_num = pwm_gpio_to_slice_num(BUZZER_PIN);
            uint frequency = 300 + (fan_speed * 2); // Frequência varia de 100 Hz a 2600 Hz
            uint wrap = 10000000 / frequency;       // Calcula o wrap com base na frequência
            if (wrap < 100)
                wrap = 100; // Limita o valor mínimo do wrap
            pwm_set_wrap(slice_num, wrap);

            uint duty_cycle = wrap / 10; // Define o ciclo de trabalho como 10% do wrap
            pwm_set_gpio_level(BUZZER_PIN, duty_cycle);
        }
        else
        {
            // Desliga o buzzer completamente
            uint slice_num = pwm_gpio_to_slice_num(BUZZER_PIN);
            pwm_set_gpio_level(BUZZER_PIN, 0); // Define o nível do PWM como 0
        }

        sleep_ms(100); // Reduz o uso da CPU
    }

    // Desligar a arquitetura CYW43.
    cyw43_arch_deinit();
    return 0;
}

// -------------------------------------- Funções ---------------------------------

// Função de callback ao aceitar conexões TCP
static err_t tcp_server_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    if (err != ERR_OK || !newpcb)
    {
        printf("Accept Erro %d\n", err);
        return ERR_VAL;
    }
    printf("Nova conexao TCP aceita do IP: %s\n", ipaddr_ntoa(&newpcb->remote_ip));
    tcp_arg(newpcb, NULL);
    tcp_recv(newpcb, tcp_server_recv);
    return ERR_OK;
}

// Tratamento do request do usuário - digite aqui

void user_request(char **request)
{
    if (strstr(*request, "GET /fan_on") != NULL)
    {
        fan_on = true;
    }
    else if (strstr(*request, "GET /fan_off") != NULL)
    {
        fan_on = false;
        fan_speed = 0;
    }
    else if (strstr(*request, "GET /fan_speed=") != NULL)
    {
        char *speed_str = strstr(*request, "fan_speed=") + 10;
        if (speed_str && isdigit((unsigned char)speed_str[0]))
        {
            fan_speed = atoi(speed_str);
            if (fan_speed > 255)
                fan_speed = 255;
        }
    }
    else if (strstr(*request, "GET /auto_fan_on") != NULL)
    {
        auto_fan = true;
    }
    else if (strstr(*request, "GET /auto_fan_off") != NULL)
    {
        auto_fan = false;
    }
    else if (strstr(*request, "GET /status") != NULL)
    {
        
    }

}

// Função de callback para processar requisições HTTP
static err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    if (err != ERR_OK && err != ERR_ABRT)
    {
        printf("Recv Erro %d\n", err);
        if (p)
            pbuf_free(p);
        tcp_close(tpcb);
        return err;
    }
    if (!p)
    {
        tcp_close(tpcb);
        tcp_recv(tpcb, NULL);
        return ERR_OK;
    }

    // Alocação do request na memória dinámica
    char *request = (char *)malloc(p->len + 1);
    memcpy(request, p->payload, p->len);
    request[p->len] = '\0';

    printf("Request: %s\n", request);

    // Tratamento de request - Controle dos LEDs
    user_request(&request);

    // Cria a resposta HTML
    char html[4096];
    // Instruções html do webserver
    if (auto_fan)
    {
        snprintf(html, sizeof(html),
                 "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n"
                 "<!DOCTYPE html><html><head><title>Controle de Ventilacao</title><style>"
                 "body{font-family:Arial;text-align:center;margin-top:50px;}button,input[type='range'],input[type='number']{font-size:20px;margin:10px;}"
                 "</style>"
                 "<script>"
                 "function atualizarValores(){"
                 "fetch('/status').then(r=>r.json()).then(d=>{"
                 "document.getElementById('temp_display').innerText=d.temp.toFixed(1);"
                 "document.getElementById('speed_display').innerText=d.speed;"
                 "});"
                 "}"
                 "</script>"
                 "</head>"
                 "<body><h1>Controle de Ventilacao</h1>"
                 "<p>Estado: %s</p>"
                 "<p>Modo automatico: %s</p>"
                 "<form action=\"/fan_on\"><button>Ligar Ventilador</button></form>"
                 "<form action=\"/fan_off\"><button>Desligar Ventilador</button></form>"
                 "<form action=\"/auto_fan_off\"><button>Desativar Ventilacao Automatica</button></form>"
                 "<p>Temperatura simulada: <b><span id='temp_display'>%.1f</span> &deg;C</b></p>"
                 "<p>Velocidade automatica: <span id='speed_display'>%d</span></p>"
                 "<form action=\"/status\"> <button>Atualizar Valores</button>"
                 "</body></html>",
                 fan_on ? "Ligado" : "Desligado",
                 "Ativado",
                 temp_c, fan_speed);
    }
    else
    {
        snprintf(html, sizeof(html),
                 "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n"
                 "<!DOCTYPE html><html><head><title>Controle de Ventilacao</title><style>"
                 "body{font-family:Arial;text-align:center;margin-top:50px;}button,input[type='range'],input[type='number']{font-size:20px;margin:10px;}"
                 "</style>"
                 "<script>let selectedSpeed=0;"
                 "function setSpeed(value){selectedSpeed=value;document.getElementById('speed_display').innerText=value;}"
                 "function sendSpeed(){fetch('/fan_speed='+selectedSpeed);alert('Velocidade enviada: '+selectedSpeed);}</script></head>"
                 "<body><h1>Controle de Ventilacao</h1>"
                 "<p>Estado: %s</p>"
                 "<p>Modo automatico: %s</p>"
                 "<form action=\"/fan_on\"><button>Ligar Ventilador</button></form>"
                 "<form action=\"/fan_off\"><button>Desligar Ventilador</button></form>"
                 "<form action=\"/auto_fan_on\"><button>Ativar Ventilacao Automatica</button></form>"
                 "<p>Velocidade: <span id='speed_display'>%d</span></p>"
                 "<input type=\"range\" min=\"0\" max=\"255\" value=\"%d\" oninput=\"setSpeed(this.value)\"><br>"
                 "<button onclick=\"sendSpeed()\" type=\"button\">Enviar Velocidade</button>"
                 "</body></html>",
                 fan_on ? "Ligado" : "Desligado",
                 "Desativado",
                 fan_speed, fan_speed);
    }
    // ...existing code...// ...existing code...// Escreve dados para envio (mas não os envia imediatamente).
    tcp_write(tpcb, html, strlen(html), TCP_WRITE_FLAG_COPY);

    // Envia a mensagem
    tcp_output(tpcb);

    // libera memória alocada dinamicamente
    free(request);

    // libera um buffer de pacote (pbuf) que foi alocado anteriormente
    pbuf_free(p);

    return ERR_OK;
}

static void gpio_irq_handler(uint gpio, uint32_t events)
{
    if (gpio == PIN_BUTTON_B)
    {
        printf("Botão B pressionado - Reiniciando para BOOTSEL\n");
        reset_usb_boot(0, 0);
    }
}