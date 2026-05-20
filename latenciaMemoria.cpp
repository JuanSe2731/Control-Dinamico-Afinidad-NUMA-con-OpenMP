#include <iostream>
#include <vector>
#include <algorithm>
#include <numeric>
#include <x86intrin.h> // Para __rdtsc()
#include <pthread.h>

// Estructura de un nodo que ocupa exactamente una línea de caché (64 bytes)
// Esto evita que el prefetcher traiga varios nodos a la vez.
struct Node {
    Node* next;
    long long pad[7]; // Relleno para completar 64 bytes
};

void measure_latency(size_t num_elements) {
    // 1. Asignar memoria
    std::vector<Node> buffer(num_elements);
    std::vector<size_t> indices(num_elements);
    std::iota(indices.begin(), indices.end(), 0);

    // 2. Aleatorizar los índices para romper el prefetcher
    std::random_shuffle(indices.begin(), indices.end());

    // 3. Crear la cadena de punteros (Pointer Chasing)
    for (size_t i = 0; i < num_elements - 1; ++i) {
        buffer[indices[i]].next = &buffer[indices[i + 1]];
    }
    buffer[indices[num_elements - 1]].next = &buffer[indices[0]]; // Cerrar el ciclo

    // 4. Calentamiento (Warming up)
    Node* current = &buffer[indices[0]];
    for (int i = 0; i < 1000000; ++i) {
        current = current->next;
    }

    // 5. Medición con RDTSC
    unsigned int junk;
    unsigned long long start = __rdtscp(&junk); // Serialización con rdtscp

    const int iterations = 100000000;
    for (int i = 0; i < iterations; ++i) {
        current = current->next;
    }

    unsigned long long end = __rdtscp(&junk);

    // 6. Resultado
    double total_cycles = static_cast<double>(end - start);
    double latency = total_cycles / iterations;

    std::cout << "Latencia promedio: " << latency << " ciclos por salto." << std::endl;
    // Para evitar que el compilador optimice y elimine el ciclo:
    if (current == nullptr) std::cout << "Esto nunca pasará" << std::endl;
}

int main() {
    // Definir tamaño: 128MB / 64 bytes por nodo ≈ 2 millones de nodos
    size_t elements = (128 * 1024 * 1024) / sizeof(Node);
    
    std::cout << "Iniciando Pointer Chasing con " << elements << " nodos..." << std::endl;
    measure_latency(elements);

    return 0;
}
