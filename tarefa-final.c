#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "pico/stdlib.h"
#include "pico/binary_info.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include "hardware/i2c.h"
#include "hardware/adc.h"
#include "hardware/pwm.h"
#include "inc/ssd1306.h"

// Definições dos pinos
#define BUTTONA_PIN 5
#define BUTTONB_PIN 6
#define JOYSTICK_BUTTON 22 
#define RED_PIN 13
#define BUZZER 21
#define EIXO_Y 26  // ADC0 - (Utilizado para BPM e para ajuste do alarme)
#define EIXO_X 27  // ADC1 - Utilizado para modo de ajuste (horas ou minutos)
#define PWM_WRAP 4095    // 12 bits (4096 valores)

// Variáveis do I2C
const uint I2C_SDA = 14;
const uint I2C_SCL = 15;

// Constantes para o ADC
#define ADC_VREF 3.3f
#define ADC_RANGE 4096
#define ADC_CONVERT (ADC_VREF / (ADC_RANGE - 1))

// Constantes para o menu
#define MENU_ITEMS 2
#define DEBOUNCE_TIME_US 200000 // 200 ms debounce

// Limites para alertas dos sensores
#define MIN_BPM 50
#define MAX_BPM 80
#define CRIT_MIN_BPM 40  
#define CRIT_MAX_BPM 120 
#define GIROSCOPIO_QUEDA_MIN 500  
#define GIROSCOPIO_QUEDA_MAX 3500
#define GIROSCOPIO_INCLINADO_MIN 1000
#define GIROSCOPIO_INCLINADO_MAX 3000 

// Definições para o sistema de média móvel e histerese
#define AMOSTRAS_BPM 10                 // Número de amostras para média móvel
#define TEMPO_HISTERESE_MS 2000         // Tempo mínimo em estado de alarme para acionar
#define INTERVALO_AMOSTRAGEM_MS 200     // Intervalo entre amostras (200ms = 5 amostras/segundo)

// Variáveis globais de controle de menu e alertas
absolute_time_t last_interrupt_time = 0;
volatile bool menu_active = true;
volatile bool submenu_active = false;
volatile uint8_t menu_index = 0;
volatile uint8_t submenu_index = 0;
volatile bool button_pressed = false;
volatile bool alerta_ativo = false;
char menu_items[MENU_ITEMS][16] = {
    "1. Monitorar",
    "2. Alarmes"
};

// Tipo de alerta
enum TipoAlerta {
    SEM_ALERTA,
    BATIMENTO_BAIXO,
    BATIMENTO_ALTO,
    QUEDA_DETECTADA,
    ALARME_TEMPORIZADOR,
    SOS_ALARME       
};

volatile enum TipoAlerta alerta_atual = SEM_ALERTA;

// Variáveis para sensores
uint16_t adc_x = 0;  // Para leitura do giroscópio e também para ajuste do timer
uint16_t adc_y = 0;  // Para leitura do batimento e também para ajuste do timer
uint8_t bpm = 65;    
uint8_t bpm_instantaneo = 65;

// Variáveis para o sistema de média móvel e histerese
uint8_t buffer_bpm[AMOSTRAS_BPM];      // Buffer circular para armazenar amostras de BPM
uint8_t indice_buffer = 0;             // Índice atual no buffer circular
uint8_t amostras_coletadas = 0;        // Contador de amostras coletadas
uint8_t media_bpm = 65;                // Média atual de BPM

// Variáveis para controle de histerese
uint32_t inicio_estado_critico_ms = 0;  // Timestamp de quando o estado crítico começou
bool estado_critico = false;            // Flag para indicar se está em estado crítico
uint32_t ultimo_tempo_amostragem = 0;   // Último tempo em que uma amostra foi coletada

// --- Variáveis para o alarme configurável ---
volatile bool alarm_active = false;                  // Indica se o alarme já foi confirmado e está em contagem
volatile uint32_t alarm_set_seconds = 60;            // Tempo configurado (inicia com 1 minuto)
volatile uint32_t alarm_trigger_time_ms = 0;         // Momento (em ms) em que o alarme disparará
volatile bool confirm_alarm = false;                 // Flag definida ao confirmar a configuração
uint32_t last_alarm_input_time = 0;                  // Debounce para ajustes do alarme
bool adjust_hours = false;                           

// Função para adicionar uma nova amostra de BPM e calcular a média móvel
void atualizar_media_bpm(uint8_t nova_amostra) {
    // Adiciona a nova amostra ao buffer circular
    buffer_bpm[indice_buffer] = nova_amostra;
    indice_buffer = (indice_buffer + 1) % AMOSTRAS_BPM;
    
    // Incrementa contador de amostras coletadas (até o máximo do tamanho do buffer)
    if (amostras_coletadas < AMOSTRAS_BPM) {
        amostras_coletadas++;
    }
    
    // Calcula a média das amostras coletadas
    uint16_t soma = 0;
    for (uint8_t i = 0; i < amostras_coletadas; i++) {
        soma += buffer_bpm[i];
    }
    
    media_bpm = soma / amostras_coletadas;
}

// Inicializa o sistema de média móvel de BPM
void inicializar_sistema_bpm() {
    // Inicializa o buffer de BPM com o valor padrão
    for (int i = 0; i < AMOSTRAS_BPM; i++) {
        buffer_bpm[i] = 65;  // Valor inicial de BPM
    }
    
    ultimo_tempo_amostragem = to_ms_since_boot(get_absolute_time());
}

// Função de callback para interrupções dos botões
void gpio_callback(uint gpio, uint32_t events) {
    absolute_time_t current_time = get_absolute_time();
    if (absolute_time_diff_us(last_interrupt_time, current_time) < DEBOUNCE_TIME_US) {
        return;
    }
    last_interrupt_time = current_time;
    
    if (gpio == BUTTONA_PIN) {
        if (menu_active && !submenu_active && !alerta_ativo) {
            // Entra no submenu a partir do menu principal
            submenu_active = true;
            submenu_index = menu_index;
        } else if (alerta_ativo) {
            // Desativa alerta (qualquer tipo)
            alerta_ativo = false;
            alerta_atual = SEM_ALERTA;
            gpio_put(RED_PIN, 0);
            pwm_set_gpio_level(BUZZER, 0);
        } else if (submenu_active && submenu_index == 1) {
            if (alarm_active) {
                alarm_active = false;
                alarm_set_seconds = 60;
                gpio_put(RED_PIN, 0);
                pwm_set_gpio_level(BUZZER, 0);
            } else {
                // Se o alarme ainda não estiver ativo, botão A confirma a configuração do alarme
                confirm_alarm = true;
            }
        }
        button_pressed = true;
    } else if (gpio == BUTTONB_PIN) {
        if (submenu_active && !alerta_ativo) {
            // B: Retorna ao menu principal
            submenu_active = false;
            // Se estiver no submenu de alarme e o alarme não estiver ativo, reinicia o tempo configurado
            if (submenu_index == 1 && !alarm_active) {
                alarm_set_seconds = 60;
            }
        } else if (alerta_ativo) {
            // Desativa alerta
            alerta_ativo = false;
            alerta_atual = SEM_ALERTA;
            gpio_put(RED_PIN, 0);
            pwm_set_gpio_level(BUZZER, 0);
        }
        button_pressed = true;
    } else if (gpio == JOYSTICK_BUTTON) {
        alerta_ativo = true;
        alerta_atual = SOS_ALARME;
        gpio_put(RED_PIN, 1);
        pwm_set_gpio_level(BUZZER, PWM_WRAP / 2);
        button_pressed = true;
    }
}

// Função para verificar alertas dos sensores com histerese
void verificar_alertas() {
    if (!submenu_active || submenu_index != 0 || alerta_ativo) return;
    
    uint32_t tempo_atual = to_ms_since_boot(get_absolute_time());
    
    // Verifica se o BPM está fora dos limites críticos
    bool bpm_critico = (media_bpm < CRIT_MIN_BPM || media_bpm > CRIT_MAX_BPM);
    
    // Se não estava em estado crítico e agora está, registra o início
    if (!estado_critico && bpm_critico) {
        estado_critico = true;
        inicio_estado_critico_ms = tempo_atual;
    }
    // Se estava em estado crítico e não está mais, reseta o estado
    else if (estado_critico && !bpm_critico) {
        estado_critico = false;
    }
    
    // Se está em estado crítico e já passou o tempo de histerese, aciona o alarme
    if (estado_critico && (tempo_atual - inicio_estado_critico_ms >= TEMPO_HISTERESE_MS)) {
        if (media_bpm < CRIT_MIN_BPM) {
            alerta_ativo = true;
            alerta_atual = BATIMENTO_BAIXO;
            gpio_put(RED_PIN, 1);
            pwm_set_gpio_level(BUZZER, PWM_WRAP / 2);
        } else if (media_bpm > CRIT_MAX_BPM) {
            alerta_ativo = true;
            alerta_atual = BATIMENTO_ALTO;
            gpio_put(RED_PIN, 1);
            pwm_set_gpio_level(BUZZER, PWM_WRAP / 2);
        }
    }
    
    // Verificação da queda permanece a mesma
    if (adc_x < GIROSCOPIO_QUEDA_MIN || adc_x > GIROSCOPIO_QUEDA_MAX) {
        alerta_ativo = true;
        alerta_atual = QUEDA_DETECTADA;
        gpio_put(RED_PIN, 1);
        pwm_set_gpio_level(BUZZER, PWM_WRAP / 2);
    }
}

// Função para ler os sensores com conversão e atualização da média móvel
void read_sensors() {
    adc_select_input(1);
    adc_x = adc_read();
    
    adc_select_input(0);
    adc_y = adc_read();
    
    uint32_t tempo_atual = to_ms_since_boot(get_absolute_time());
    
    // Calcula o BPM instantâneo com base na leitura do ADC
    if (adc_y < 1000) {
        bpm_instantaneo = (uint8_t)(adc_y * 0.04);
    } else if (adc_y > 3000) {
        float excesso = adc_y - 3000;
        bpm_instantaneo = 80 + (uint8_t)(excesso * 0.04);
    } else {
        bpm_instantaneo = 50 + (uint8_t)((adc_y - 1000) * 0.015);
    }
    
    // Atualiza a média móvel em intervalos regulares
    if (tempo_atual - ultimo_tempo_amostragem >= INTERVALO_AMOSTRAGEM_MS) {
        atualizar_media_bpm(bpm_instantaneo);
        ultimo_tempo_amostragem = tempo_atual;
    }
    
    // Atualiza o valor de BPM global para exibição com a média calculada
    bpm = media_bpm;
    
    verificar_alertas();
}

// Processa a navegação do menu principal
bool process_joystick_navigation() {
    read_sensors();
    if (menu_active && !submenu_active && !alerta_ativo) {
        if (adc_y < 1000) {
            if (menu_index > 0) {
                menu_index--;
                return true;
            }
        } else if (adc_y > 3000) {
            if (menu_index < MENU_ITEMS - 1) {
                menu_index++;
                return true;
            }
        }
    }
    return false;
}

// Processa os ajustes do tempo do alarme no submenu "Alarmes" utilizando ambos os eixos
void process_alarm_input() {
    if (!(submenu_active && submenu_index == 1 && !alarm_active)) return;
    
    uint32_t current_time = to_ms_since_boot(get_absolute_time());
    if (current_time - last_alarm_input_time < 250) return;
    last_alarm_input_time = current_time;
    
    // Leitura dos eixos do joystick para configuração:
    // Eixo Y (ADC0) para ajuste do timer (inverso: up = decremento, down = incremento)
    adc_select_input(0);
    uint16_t joystick_y = adc_read();
    // Eixo X (ADC1) para seleção do modo de ajuste (horas ou minutos)
    adc_select_input(1);
    uint16_t joystick_x = adc_read();
    
    // Define o modo de ajuste com base no eixo X:
    if (joystick_x < 1000) {
         adjust_hours = true;
    } else if (joystick_x > 3000) {
         adjust_hours = false;
    }
    
    // Determina o incremento: 3600 segundos para horas ou 60 segundos para minutos.
    uint32_t incremento = adjust_hours ? 3600 : 60;
    if (joystick_y < 1000) {
         if (alarm_set_seconds > 60 + incremento - 1) {
             alarm_set_seconds -= incremento;
         } else {
             alarm_set_seconds = 60;
         }
    } else if (joystick_y > 3000) {
         if (alarm_set_seconds + incremento <= 28800) {
             alarm_set_seconds += incremento;
         } else {
             alarm_set_seconds = 28800;
         }
    }
}

// Função auxiliar para atualizar o display
void process_command(char *line1, char *line2, char *line3, char *line4, uint8_t *ssd, struct render_area *frame_area) {
    memset(ssd, 0, ssd1306_buffer_length);
    render_on_display(ssd, frame_area);
    
    if (line1 != NULL) ssd1306_draw_string(ssd, 5, 0, line1);
    if (line2 != NULL) ssd1306_draw_string(ssd, 5, 8, line2);
    if (line3 != NULL) ssd1306_draw_string(ssd, 5, 16, line3);
    if (line4 != NULL) ssd1306_draw_string(ssd, 5, 24, line4);
    
    render_on_display(ssd, frame_area);
}

// Desenha o menu principal
void draw_menu(uint8_t *ssd, struct render_area *frame_area) {
    char line1[32] = "MENU PRINCIPAL";
    char line2[32] = "";
    char line3[32] = "";
    char line4[32] = "";
    
    for (int i = 0; i < MENU_ITEMS; i++) {
        if (i == menu_index) {
            if (i == 0) {
                snprintf(line2, sizeof(line2), "l %s", menu_items[i]);
            } else if (i == 1) {
                snprintf(line3, sizeof(line3), "l %s", menu_items[i]);
            }
        } else {
            if (i == 0) {
                snprintf(line2, sizeof(line2), "   %s", menu_items[i]);
            } else if (i == 1) {
                snprintf(line3, sizeof(line3), "   %s", menu_items[i]);
            }
        }
    }
    
    process_command(line1, line2, line3, line4, ssd, frame_area);
}

// Submenu de monitoramento - modificado para mostrar BPM atual e média
void draw_submenu_adc(uint8_t *ssd, struct render_area *frame_area) {
    char line1[32] = "MONITORAMENTO";
    char line2[32] = "";
    char line3[32] = "";
    char line4[32] = "";
    
    // Mostra tanto o BPM atual quanto a média
    snprintf(line2, sizeof(line2), "BPM %d Med %d ", bpm_instantaneo, bpm);
    
    if (adc_x < GIROSCOPIO_QUEDA_MIN || adc_x > GIROSCOPIO_QUEDA_MAX) {
        snprintf(line3, sizeof(line3), "Giro: ALERTA!");
    } else if ((adc_x > GIROSCOPIO_INCLINADO_MAX && adc_x < GIROSCOPIO_QUEDA_MAX) || (adc_x < GIROSCOPIO_INCLINADO_MIN && adc_x > GIROSCOPIO_QUEDA_MIN)) {
        snprintf(line3, sizeof(line3), "Giro: Inclinado");
    } else {
        snprintf(line3, sizeof(line3), "Giro: Normal");
    }
    snprintf(line4, sizeof(line4), "B:Voltar");
    
    process_command(line1, line2, line3, line4, ssd, frame_area);
}

// Submenu de alarmes com ajuste de tempo, exibição do modo e status
void draw_submenu_alarmes(uint8_t *ssd, struct render_area *frame_area) {
    char line1[32] = "ALARMES";
    char line2[32] = "";
    char line3[32] = "";
    char line4[32] = "";
    
    if (!alarm_active) {
        uint32_t horas = alarm_set_seconds / 3600;
        uint32_t minutos = (alarm_set_seconds % 3600) / 60;
        uint32_t segundos = alarm_set_seconds % 60;
        snprintf(line2, sizeof(line2), "Tempo %02u:%02u:%02u [%s]", horas, minutos, segundos, adjust_hours ? "H" : "M");
        snprintf(line3, sizeof(line3), "A Confirmar");
        snprintf(line4, sizeof(line4), "B Voltar");
    } else {
        uint32_t current_ms = to_ms_since_boot(get_absolute_time());
        uint32_t restante = (alarm_trigger_time_ms > current_ms) ? (alarm_trigger_time_ms - current_ms) / 1000 : 0;
        uint32_t horas = restante / 3600;
        uint32_t minutos = (restante % 3600) / 60;
        uint32_t segundos = restante % 60;
        snprintf(line2, sizeof(line2), "Restante");
        snprintf(line3, sizeof(line3), "%02u:%02u:%02u", horas, minutos, segundos);
        snprintf(line4, sizeof(line4), "BVoltar ACancelar");
    }
    
    process_command(line1, line2, line3, line4, ssd, frame_area);
}

// Tela de alerta (exibe o tipo de alerta)
void draw_alerta(uint8_t *ssd, struct render_area *frame_area) {
    char line1[32] = "ALERTA";
    char line2[32] = "";
    char line3[32] = "";
    char line4[32] = "Pressione A";
    
    switch (alerta_atual) {
        case BATIMENTO_BAIXO:
            snprintf(line2, sizeof(line2), "BATIMENTO BAIXO");
            snprintf(line3, sizeof(line3), "BPM: %d", media_bpm);
            break;
        case BATIMENTO_ALTO:
            snprintf(line2, sizeof(line2), "BATIMENTO ALTO");
            snprintf(line3, sizeof(line3), "BPM: %d", media_bpm);
            break;
        case QUEDA_DETECTADA:
            snprintf(line2, sizeof(line2), "QUEDA DETECTADA");
            break;
        case ALARME_TEMPORIZADOR:
            snprintf(line2, sizeof(line2), "ALARME!");
            snprintf(line3, sizeof(line3), "Tempo esgotado");
            snprintf(line4, sizeof(line4), "Hora do remedio");
            break;
        case SOS_ALARME:
            snprintf(line2, sizeof(line2), "SOS ALARME");
            snprintf(line3, sizeof(line3), "Ativado");
            break;
        default:
            snprintf(line2, sizeof(line2), "ERRO DESCONHECIDO");
            break;
    }
    
    process_command(line1, line2, line3, line4, ssd, frame_area);
}

int main() {
    
    // Inicialização do I2C e OLED
    i2c_init(i2c1, ssd1306_i2c_clock * 4000);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);
    ssd1306_init();
    
    struct render_area frame_area = {
        .start_column = 0,
        .end_column = ssd1306_width - 1,
        .start_page = 0,
        .end_page = ssd1306_n_pages - 1
    };
    calculate_render_area_buffer_length(&frame_area);
    
    uint8_t ssd[ssd1306_buffer_length];
    memset(ssd, 0, ssd1306_buffer_length);
    render_on_display(ssd, &frame_area);
    
    // Configuração do PWM para o buzzer
    gpio_set_function(BUZZER, GPIO_FUNC_PWM);
    uint slice1 = pwm_gpio_to_slice_num(BUZZER);
    pwm_set_wrap(slice1, PWM_WRAP);
    pwm_set_clkdiv(slice1, 30.0f);
    pwm_set_chan_level(slice1, PWM_CHAN_A, 0);
    pwm_set_enabled(slice1, true);
    
    // Configuração dos botões
    gpio_init(BUTTONA_PIN);
    gpio_set_dir(BUTTONA_PIN, GPIO_IN);
    gpio_pull_up(BUTTONA_PIN);
    
    gpio_init(BUTTONB_PIN);
    gpio_set_dir(BUTTONB_PIN, GPIO_IN);
    gpio_pull_up(BUTTONB_PIN);
    
    // Configuração do botão SOS (JOYSTICK_BUTTON)
    gpio_init(JOYSTICK_BUTTON);
    gpio_set_dir(JOYSTICK_BUTTON, GPIO_IN);
    gpio_pull_up(JOYSTICK_BUTTON);
    
    // Inicialização do LED vermelho
    gpio_init(RED_PIN);
    gpio_set_dir(RED_PIN, GPIO_OUT);
    gpio_put(RED_PIN, 0);
    
    // Configuração das interrupções
    gpio_set_irq_enabled(BUTTONA_PIN, GPIO_IRQ_EDGE_FALL, true);
    gpio_set_irq_enabled(BUTTONB_PIN, GPIO_IRQ_EDGE_FALL, true);
    gpio_set_irq_enabled(JOYSTICK_BUTTON, GPIO_IRQ_EDGE_FALL, true);
    gpio_set_irq_callback(gpio_callback);
    irq_set_enabled(IO_IRQ_BANK0, true);
    
    // Inicialização do ADC
    adc_init();
    adc_gpio_init(EIXO_X);
    adc_gpio_init(EIXO_Y);
    
    // Inicialização do sistema de média móvel para BPM
    inicializar_sistema_bpm();
    
    // Mensagem inicial
    process_command("Inicializando...", "Sistema de", "Monitoramento", "de Saude", ssd, &frame_area);
    sleep_ms(2000);
    
    // Exibe o menu principal
    draw_menu(ssd, &frame_area);
    
    bool update_display = false;
    uint32_t last_adc_update = 0;
    
    while(1) {
        uint32_t current_time = to_ms_since_boot(get_absolute_time());
        
        // Verifica se o alarme ativo atingiu o tempo definido
        if (alarm_active && current_time >= alarm_trigger_time_ms) {
            alerta_ativo = true;
            alerta_atual = ALARME_TEMPORIZADOR;
            alarm_active = false;
            // Ativa LED e buzzer
            gpio_put(RED_PIN, 1);
            pwm_set_gpio_level(BUZZER, PWM_WRAP / 2);
        }
        
        if (alerta_ativo) {
            draw_alerta(ssd, &frame_area);
            sleep_ms(50);
            continue;
        }
        
        // Se estivermos no submenu de "Alarmes" e o alarme não estiver ativo, processa os ajustes com o joystick
        if (submenu_active && submenu_index == 1 && !alarm_active) {
            process_alarm_input();
        }
        
        update_display = process_joystick_navigation();
        
        // Se o usuário confirmou a configuração do alarme, ativa-o
        if (confirm_alarm && submenu_active && submenu_index == 1 && !alarm_active) {
            alarm_active = true;
            alarm_trigger_time_ms = current_time + alarm_set_seconds * 1000;
            confirm_alarm = false;
        }
        
        if ((submenu_active || button_pressed) && (current_time - last_adc_update > 250)) {
            update_display = true;
            last_adc_update = current_time;
        }
        
        if (update_display || button_pressed) {
            if (submenu_active) {
                if (submenu_index == 0) {
                    draw_submenu_adc(ssd, &frame_area);
                } else if (submenu_index == 1) {
                    draw_submenu_alarmes(ssd, &frame_area);
                }
            } else {
                draw_menu(ssd, &frame_area);
            }
            button_pressed = false;
            update_display = false;
        }
        
        sleep_ms(30);
    }
    
    return 0;
}