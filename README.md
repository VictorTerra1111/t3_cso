# C-LOOK: módulo escalonador de disco

## Parâmetros
O módulo de kernel é parametrizável, de tal forma que a configuração do módulo possa ser definida em tempo de carga. Os seguintes parâmetros precisam ser definidos, sendo que estes irão influenciar o comportamento do módulo:

* Tamanho da fila de requisições: Define o número de requisições que deverão ser enfileiradas antes de se realizar o despacho das mesmas. Esse parâmetro especifica a granularidade com a qual o algoritmo irá trabalhar, ou seja, quando a fila ficar cheia devem ser atendidas as solicitações. Recomenda-se usar um valor entre 20 e 100 requisições.
  
* Tempo máximo de espera (em milisegundos): Define o tempo máximo em que requisições que estão na fila devem ser atendidas. Passado o tempo de espera após a última requisição, mesmo que a fila não esteja cheia as requisições nela armazenadas deverão ser atendidas. Recomenda-se um valor entre 20ms e 100ms.

* Modo de depuração: caso habilitado, mensagens sobre o comportamento do algoritmo deverão ser exibidas no log do kernel. Por exemplo, requisições adicionadas na fila, requisições na ordem em que são atendidas, tempo dos eventos, entre outros. É importante que não sejam geradas mensagens de depuração no terminal, apenas no log do kernel.

## Avaliação de desempenho

Se as requisições forem reordenadas segundo o algoritmo C-LOOK, teremos uma redução no total de setores percorridos. A ordem de chegada das requisições ao módulo equivale ao comportamento do escalonador FCFS (First Come, First Served), pois sem reordenação as requisições seriam atendidas exatamente na sequência em que chegaram. Desta forma, caso a depuração esteja habilitada, a implementação do módulo deve exibir os setores na ordem de chegada (que representa o comportamento FCFS) e na ordem em que forem sendo atendidos (ordem C-LOOK), indicando também quando ocorre o salto circular (retorno ao menor setor pendente após atender o maior), e ao final uma comparação entre o número total de setores que seriam percorridos na ordem de chegada (FCFS) e o número efetivamente percorrido com a reordenação C-LOOK. Essa comparação é extraída de uma única execução com o módulo C-LOOK carregado. Não é necessário realizar uma segunda execução com o escalonador none (FCFS) para obter os dados de referência, pois a própria ordem de chegada reportada pelo módulo já representa o comportamento FCFS. Assim, é importante gerar um grande número de requisições de disco com a aplicação de teste, evitando que o escalonador fique sem trabalho a fazer. A forma recomendada é criar uma aplicação de teste que utilize fork() para criar um grande número de processos que geram requisições de leitura e escrita em regiões aleatórias do disco.

Atente-se para gerar requisições de leitura e escrita, e que a faixa de valores das requisições cubram todo o tamanho do disco virtual utilizado. Por exemplo, se um disco de 4MB estiver sendo utilizado e forem considerados blocos com tamanho de 4096 bytes, devem ser geradas requisições com valores (número do bloco) entre 0 e 1023. Dessa forma, os seguintes argumentos precisam ser definidos e passados em tempo de execução para aplicação de testes:

* Tamanho do bloco (em bytes, potência de dois);
* Tamanho do disco (em blocos);
* Número de operações de entrada e saı́da;
* Percentual de escritas (valor entre 0 e 100, sendo o restante operações de leitura);
* Tamanho mı́nimo e máximo de cada requisição (valor menor ou igual ao tamanho do bloco);
* Número de processos concorrentes (gerados a partir do fork).

## Teste

O relatório deverá apresentar resultados que ilustram o funcionamento do algoritmo e sua comparação com a polı́tica FCFS, com análise e discussão dos resultados obtidos em cada caso. Como descrito na seção de Avaliação de desempenho, os dados do FCFS são obtidos da mesma execução com o módulo C-LOOK — a ordem de chegada registrada no log de depuração representa o comportamento FCFS. Não é necessário (nem correto) realizar uma execução separada com o escalonador none para obter os dados de comparação. Os casos de teste a seguir devem ser executados com o módulo C-LOOK e os resultados comparados (setores na ordem de chegada vs. setores na ordem C-LOOK):

* Acesso aleatório puro: grande número de operações com endereços distribuídos uniformemente por todo o disco, alta concorrência. Discuta por que o ganho do C-LOOK em relação ao FCFS é significativo nesse padrão e como a varredura unidirecional circular contribui para isso.
* Acesso sequencial: operações com endereços crescentes e sequenciais. Discuta por que o ganho do C-LOOK em relação ao FCFS é menor nesse padrão.
* Fila pequena vs. fila grande: execute o caso 1 alterando o tamanho da fila entre os extremos recomendados. Analise como esse parâmetro influencia a redução de setores percorridos.
* Timeout curto vs. timeout longo: execute o caso 1 alterando o tempo máximo de espera entre os extremos recomendados. Discuta o compromisso entre tempo de espera e eficiência do escalonamento.
* Carga mista com predominância de escritas: repita o caso 1 com percentuais elevados de escritas. Discuta se e por que o comportamento difere do caso com predominância de leituras.

Para cada caso, o relatório deve incluir: a configuração utilizada (parâmetros do módulo e da aplicação de testes), os resultados numéricos obtidos (setores percorridos com e sem reordenação), e uma discussão que explique o comportamento observado com base no funcionamento dos algoritmos.
