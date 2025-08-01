#include <linux/module.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/uaccess.h> // Incluída para o sscanf

MODULE_AUTHOR("DevTITANS <devtitans@icomp.ufam.edu.br>");
MODULE_DESCRIPTION("Driver de acesso ao SmartLamp (ESP32 com Chip Serial CP2102");
MODULE_LICENSE("GPL");

#define MAX_RECV_LINE 100 // Tamanho máximo de uma linha de resposta do dispositivo USB

// Variáveis globais do driver
static char recv_line[MAX_RECV_LINE];              // Armazena dados vindos da USB até receber um caractere de nova linha '\n'
static struct usb_device *smartlamp_device;        // Referência para o dispositivo USB
static uint usb_in, usb_out;                       // Endereços das portas de entrada e saida da USB
static char *usb_in_buffer, *usb_out_buffer;       // Buffers de entrada e saída da USB
static char *cmd_buffer;                            // Buffer para montar o comando completo
static int usb_max_size;                           // Tamanho máximo de uma mensagem USB

// Informações de identificação do dispositivo USB (Vendor ID e Product ID)
#define VENDOR_ID   0x10c4
#define PRODUCT_ID  0xea60
static const struct usb_device_id id_table[] = { { USB_DEVICE(VENDOR_ID, PRODUCT_ID) }, {} };

// Protótipos das funções
static int  usb_probe(struct usb_interface *ifce, const struct usb_device_id *id); // Executado quando o dispositivo é conectado na USB
static void usb_disconnect(struct usb_interface *ifce);                           // Executado quando o dispositivo USB é desconectado da USB
static int  usb_send_cmd(char *cmd, int param, void *result_ptr);                 // Envia um comando para o dispositivo e processa a resposta

// Funções para manipular os arquivos no /sys/kernel/smartlamp
static ssize_t attr_show(struct kobject *sys_obj, struct kobj_attribute *attr, char *buff); // Executado quando o arquivo é lido (e.g., cat)
static ssize_t attr_store(struct kobject *sys_obj, struct kobj_attribute *attr, const char *buff, size_t count); // Executado quando o arquivo é escrito (e.g., echo)

// Variáveis para criar os arquivos no /sys/kernel/smartlamp/{led, ldr, temp, hum}
static struct kobj_attribute  led_attribute = __ATTR(led, S_IRUGO | S_IWUSR, attr_show, attr_store); // LED é leitura e escrita
static struct kobj_attribute  ldr_attribute = __ATTR(ldr, S_IRUGO, attr_show, NULL); // LDR é somente leitura, então attr_store é NULL
static struct kobj_attribute  temp_attribute = __ATTR(temp, S_IRUGO, attr_show, NULL); // Temp é somente leitura
static struct kobj_attribute  hum_attribute = __ATTR(hum, S_IRUGO, attr_show, NULL);   // Hum é somente leitura

static struct attribute      *attrs[]       = {
    &led_attribute.attr,
    &ldr_attribute.attr,
    &temp_attribute.attr,
    &hum_attribute.attr,
    NULL
};

static struct attribute_group attr_group    = { .attrs = attrs };
static struct kobject        *sys_obj;                                             // Objeto sysfs para o diretório /sys/kernel/smartlamp

// Definição do driver USB
static struct usb_driver smartlamp_driver = {
    .name        = "smartlamp",     // Nome do driver
    .probe       = usb_probe,       // Função chamada na conexão do dispositivo
    .disconnect  = usb_disconnect,  // Função chamada na desconexão do dispositivo
    .id_table    = id_table,        // Tabela com o VendorID e ProductID do dispositivo
};

module_usb_driver(smartlamp_driver);

// ---

// Executado quando o dispositivo é conectado na USB
static int usb_probe(struct usb_interface *interface, const struct usb_device_id *id) {
    struct usb_endpoint_descriptor *usb_endpoint_in, *usb_endpoint_out;
    long ldr_value;

    printk(KERN_INFO "SmartLamp: Dispositivo conectado ...\n");

    // Cria o diretório /sys/kernel/smartlamp
    sys_obj = kobject_create_and_add("smartlamp", kernel_kobj);
    if (!sys_obj) {
        printk(KERN_ERR "SmartLamp: falha ao criar o objeto sysfs\n");
        return -ENOMEM;
    }
    // Cria os arquivos (atributos) no diretório sysfs
    if (sysfs_create_group(sys_obj, &attr_group)) {
        printk(KERN_ERR "SmartLamp: falha ao criar grupo de atributos\n");
        kobject_put(sys_obj); // Limpa o objeto criado em caso de erro
        return -ENOMEM;
    }

    // Identifica e configura os endpoints de comunicação
    smartlamp_device = interface_to_usbdev(interface);
    if (usb_find_common_endpoints(interface->cur_altsetting, &usb_endpoint_in, &usb_endpoint_out, NULL, NULL)) {
        printk(KERN_ERR "SmartLamp: Falha ao encontrar endpoints.\n");
        kobject_put(sys_obj);
        return -EIO;
    }
    usb_max_size = usb_endpoint_maxp(usb_endpoint_in);
    usb_in = usb_endpoint_in->bEndpointAddress;
    usb_out = usb_endpoint_out->bEndpointAddress;

    // Aloca memória para os buffers de comunicação
    usb_in_buffer = kmalloc(usb_max_size, GFP_KERNEL);
    usb_out_buffer = kmalloc(usb_max_size, GFP_KERNEL);
    cmd_buffer = kmalloc(MAX_RECV_LINE, GFP_KERNEL);

    if (!usb_in_buffer || !usb_out_buffer || !cmd_buffer) {
        printk(KERN_ERR "SmartLamp: Falha na alocação de memória para os buffers.\n");
        kfree(usb_in_buffer);
        kfree(usb_out_buffer);
        kfree(cmd_buffer);
        kobject_put(sys_obj);
        return -ENOMEM;
    }

    // Testa a comunicação lendo o valor inicial do LDR
    if (usb_send_cmd("GET_LDR", 0, &ldr_value) >= 0) {
        printk(KERN_INFO "SmartLamp: LDR Value inicial: %ld\n", ldr_value);
    } else {
        printk(KERN_ERR "SmartLamp: Falha ao ler valor inicial do LDR\n");
    }

    return 0;
}

// ---

// Executado quando o dispositivo USB é desconectado da USB
static void usb_disconnect(struct usb_interface *interface) {
    printk(KERN_INFO "SmartLamp: Dispositivo desconectado.\n");
    if (sys_obj) kobject_put(sys_obj);      // Remove os arquivos em /sys/kernel/smartlamp
    kfree(usb_in_buffer);                   // Desaloca buffers
    kfree(usb_out_buffer);
    kfree(cmd_buffer);                      // Desaloca buffers
}

// ---

// Envia um comando via USB, espera e armazena a resposta
static int usb_send_cmd(char *cmd, int param, void *result_ptr) {
    int recv_size = 0;
    int ret, actual_size, i;
    int retries = 20;
    char resp_expected[MAX_RECV_LINE];
    char *start_of_value;

    memset(recv_line, 0, MAX_RECV_LINE);
    memset(cmd_buffer, 0, MAX_RECV_LINE);
    memset(resp_expected, 0, MAX_RECV_LINE);

    // Monta o comando de acordo com o parâmetro 'cmd'
    if (strcmp(cmd, "SET_LED") == 0) {
        snprintf(cmd_buffer, MAX_RECV_LINE, "SET_LED %d\n", param);
        snprintf(resp_expected, MAX_RECV_LINE, "RES SET_LED");
    } else if (strcmp(cmd, "GET_LED") == 0) {
        snprintf(cmd_buffer, MAX_RECV_LINE, "GET_LED\n");
        snprintf(resp_expected, MAX_RECV_LINE, "RES GET_LED");
    } else if (strcmp(cmd, "GET_LDR") == 0) {
        snprintf(cmd_buffer, MAX_RECV_LINE, "GET_LDR\n");
        snprintf(resp_expected, MAX_RECV_LINE, "RES GET_LDR");
    } else if (strcmp(cmd, "GET_TEMP") == 0) {
        snprintf(cmd_buffer, MAX_RECV_LINE, "GET_TEMP\n");
        snprintf(resp_expected, MAX_RECV_LINE, "RES GET_TEMP");
    } else if (strcmp(cmd, "GET_HUM") == 0) {
        snprintf(cmd_buffer, MAX_RECV_LINE, "GET_HUM\n");
        snprintf(resp_expected, MAX_RECV_LINE, "RES GET_HUM");
    } else {
        printk(KERN_ERR "SmartLamp: Comando desconhecido: %s\n", cmd);
        return -1;
    }
    printk(KERN_INFO "SmartLamp: Enviando comando: %s", cmd_buffer);

    // Envia o comando para o dispositivo USB
    ret = usb_bulk_msg(smartlamp_device, usb_sndbulkpipe(smartlamp_device, usb_out),
                       cmd_buffer, strlen(cmd_buffer), &actual_size, 1000);
    if (ret) {
        printk(KERN_ERR "SmartLamp: Erro de codigo %d ao enviar comando!\n", ret);
        return -1;
    }

    // Espera e lê a resposta do dispositivo
    while (retries > 0) {
        ret = usb_bulk_msg(smartlamp_device, usb_rcvbulkpipe(smartlamp_device, usb_in),
                          usb_in_buffer, min(usb_max_size, MAX_RECV_LINE), &actual_size, 1000);
        if (ret) {
            printk(KERN_ERR "SmartLamp: Erro ao ler dados da USB (tentativa %d). Codigo: %d\n", retries, ret);
            retries--;
            continue;
        }

        // Processa os dados recebidos, procurando por uma nova linha
        for (i = 0; i < actual_size; i++) {
            if (usb_in_buffer[i] == '\n' || recv_size >= MAX_RECV_LINE - 1) {
                recv_line[recv_size] = '\0';
                printk(KERN_INFO "SmartLamp: Resposta recebida: %s\n", recv_line);

                // Verifica se a resposta corresponde ao comando enviado
                if (strncmp(recv_line, resp_expected, strlen(resp_expected)) == 0) {
                    start_of_value = recv_line + strlen(resp_expected) + 1;

                    // Lógica de conversão da string da resposta para um número inteiro
                    if (strcmp(cmd, "GET_TEMP") == 0 || strcmp(cmd, "GET_HUM") == 0) {
                        int integer_part, decimal_part;
                        // O sscanf para floats causa erro no kernel. A solução é ler como inteiros.
                        if (sscanf(start_of_value, "%d.%d", &integer_part, &decimal_part) == 2) {
                            if (result_ptr) {
                                // Armazena o valor em um long, multiplicando por 100 para manter a precisão
                                *(long *)result_ptr = integer_part * 100 + decimal_part;
                                printk(KERN_INFO "SmartLamp: Valor inteiro extraído (com precisão): %ld\n", *(long *)result_ptr);
                                return 0;
                            }
                        }
                    } else { // GET_LDR, GET_LED
                        long long_value;
                        // Usa sscanf para inteiros, que é mais robusto com espaços em branco
                        if (sscanf(start_of_value, "%ld", &long_value) == 1) {
                            if (result_ptr) {
                                *(long *)result_ptr = long_value;
                                printk(KERN_INFO "SmartLamp: Valor inteiro extraído: %ld\n", long_value);
                                return 0;
                            }
                        }
                    }

                    printk(KERN_ERR "SmartLamp: Erro ao converter o valor da resposta.\n");
                    return -1;
                }
                recv_size = 0;
                memset(recv_line, 0, MAX_RECV_LINE);
            } else {
                recv_line[recv_size++] = usb_in_buffer[i];
            }
        }
        retries--;
    }

    printk(KERN_ERR "SmartLamp: Timeout - não recebeu resposta esperada\n");
    return -1;
}

// ---

// Executado quando o arquivo /sys/kernel/smartlamp/{led, ldr, temp, hum} é lido (e.g., cat /sys/kernel/smartlamp/led)
static ssize_t attr_show(struct kobject *sys_obj, struct kobj_attribute *attr, char *buff) {
    const char *attr_name = attr->attr.name;
    int ret;
    long int_value;

    printk(KERN_INFO "SmartLamp: Lendo %s ...\n", attr_name);

    // Chama a função de envio de comando e lê o valor
    if (strcmp(attr_name, "led") == 0) {
        ret = usb_send_cmd("GET_LED", 0, &int_value);
        if (ret == 0) return sprintf(buff, "%ld\n", int_value);
    } else if (strcmp(attr_name, "ldr") == 0) {
        ret = usb_send_cmd("GET_LDR", 0, &int_value);
        if (ret == 0) return sprintf(buff, "%ld\n", int_value);
    } else if (strcmp(attr_name, "temp") == 0) { // Comando GET_TEMP
        ret = usb_send_cmd("GET_TEMP", 0, &int_value);
        if (ret == 0) {
             // Formata o valor inteiro para um float de duas casas decimais
             return sprintf(buff, "%ld.%02ld\n", int_value / 100, int_value % 100);
        }
    } else if (strcmp(attr_name, "hum") == 0) {  // Comando GET_HUM
        ret = usb_send_cmd("GET_HUM", 0, &int_value);
        if (ret == 0) {
            // Formata o valor inteiro para um float de duas casas decimais
            return sprintf(buff, "%ld.%02ld\n", int_value / 100, int_value % 100);
        }
    } else {
        printk(KERN_ERR "SmartLamp: Atributo desconhecido: %s\n", attr_name);
        return -EINVAL;
    }

    printk(KERN_ERR "SmartLamp: Erro ao ler %s\n", attr_name);
    return -EIO;
}

// ---

// Executado quando o arquivo /sys/kernel/smartlamp/{led} é escrito (e.g., echo "100" | sudo tee -a /sys/kernel/smartlamp/led)
static ssize_t attr_store(struct kobject *sys_obj, struct kobj_attribute *attr, const char *buff, size_t count) {
    long ret, value;
    const char *attr_name = attr->attr.name;

    // Converte o valor recebido da string para long
    if (sscanf(buff, "%ld", &value) != 1) {
        printk(KERN_ALERT "SmartLamp: valor de %s invalido.\n", attr_name);
        return -EACCES;
    }

    // Verifica se é um atributo válido para escrita
    if (strcmp(attr_name, "led") == 0) {
        // Valida o range do LED (0-100)
        if (value < 0 || value > 100) {
            printk(KERN_ALERT "SmartLamp: valor do LED deve estar entre 0 e 100.\n");
            return -EINVAL;
        }

        printk(KERN_INFO "SmartLamp: Setando %s para %ld ...\n", attr_name, value);

        // Envia o comando SET_LED com o valor
        ret = usb_send_cmd("SET_LED", (int)value, NULL);
        if (ret < 0) {
            printk(KERN_ALERT "SmartLamp: erro ao setar o valor do %s.\n", attr_name);
            return -EIO;
        }
    } else if (strcmp(attr_name, "ldr") == 0 || strcmp(attr_name, "temp") == 0 || strcmp(attr_name, "hum") == 0) {
        // LDR, TEMP, HUM são somente leitura
        printk(KERN_ALERT "SmartLamp: %s é somente leitura.\n", attr_name);
        return -EACCES;
    } else {
        printk(KERN_ERR "SmartLamp: Atributo desconhecido: %s\n", attr_name);
        return -EINVAL;
    }

    return count;
}