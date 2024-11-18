# **Projeto PintOS - Alarm Clock e Multi-Level Feedback Queue (MFQ)**

Projeto desenvolvido com intuito de implementar as funcionalidades de Alarm Clock e Multi-Level Feedback Queue (MFQ) no sistema educacional PintOS.

## Descrição
Neste projeto, desenvolvemos duas funcionalidades para o sistema PintOS:

Alarm Clock (Timer Sleep): Permite que uma thread "durma" por um tempo específico. As threads em espera ficam organizadas para acordarem no momento correto.

Multi-Level Feedback Queue (MFQ): Implementa um escalonador com várias filas de prioridade. Threads com maior prioridade são atendidas primeiro, enquanto threads que usam mais CPU são movidas para níveis de prioridade mais baixos.

## Principais Desafios
Sincronização de threads: Usamos semáforos para evitar bloqueios e garantir que as threads acordassem no tempo correto.

Escalonamento eficiente: Implementamos ajustes dinâmicos de prioridade para que o escalonador funcione de forma justa.

## Autores
- Andreywid Yago
- Luiz Gustavo
