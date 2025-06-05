// Defina os pinos de LED e LDR
// Defina uma variável com valor máximo do LDR (4000)
// Defina uma variável para guardar o valor atual do LED (10)
int ledPin = 15;
int ledValue = 10;

int ldrPin=2;
// Faça testes no sensor ldr para encontrar o valor maximo e atribua a variável ldrMax
int ldrMax;

// Intensidade inicial (de 0 a 100)

void setup() {
  Serial.begin(9600);
  pinMode(ledPin, OUTPUT);
  pinMode(ldrPin, INPUT);

  // Inicializa LED com valor normalizado
  analogWrite(ledPin, normalizeIntensity(ledValue));

   Serial.printf("SmartLamp Initialized.");
}

void loop() {
  // Lê comando do Monitor Serial (Ctrl + Shift + M)
  if (Serial.available()) {
    String command = Serial.readStringUntil('\n');
    command.trim(); // Remove espaços em branco e quebras de linha
    processCommand(command);
  }
}

// Função para atualizar o valor do LED
void ledUpdate(String command) {
    // Valor deve convertar o valor recebido pelo comando SET_LED para 0 e 255
    // Normalize o valor do LED antes de enviar para a porta correspondente
    String valueStr = command.substring(8);
    int value = valueStr.toInt();

    if (isValidNumber(valueStr) && value >= 0 && value <= 100) {
      ledValue = value;
      analogWrite(ledPin, normalizeIntensity(ledValue));
      Serial.println("RES SET_LED 1");
    } else {
      Serial.println("RES SET_LED -1");
    }
}

// Função para ler o valor do LDR
int ldrGetValue() {
    // Leia o sensor LDR e retorne o valor normalizado entre 0 e 100
    // faça testes para encontrar o valor maximo do ldr (exemplo: aponte a lanterna do celular para o sensor)       
    // Atribua o valor para a variável ldrMax e utilize esse valor para a normalização
    int ldrValue = analogRead(ldrPin);
    return map(ldrValue, 0, 100, 0, 4096);    
}


void processCommand(String command) {
  // compare o comando com os comandos possíveis e execute a ação correspondente    
  if (command.startsWith("SET_LED ")) {
      ledUpdate(command);
  }
  else if (command == "GET_LED") {
    Serial.print("RES GET_LED ");
    Serial.println(ldrGetValue());
  }
  else if (command == "GET_LDR") {
        
        Serial.print("RES GET_LDR ");    
        int ldrValue = analogRead(ldrPin);
        Serial.println(map(ldrValue, 0, 4096, 0, 100));    
  }
  else {
    Serial.println("ERR Unknown command.");
  }
}

// Normaliza valor de 0–100 para 0–255
int normalizeIntensity(int val) {
  return map(val, 0, 100, 0, 255);
}

// Verifica se a string é um número válido
bool isValidNumber(String str) {
  if (str.length() == 0) return false;
  for (int i = 0; i < str.length(); i++) {
    if (!isDigit(str.charAt(i))) return false;
  }
  return true;
}
