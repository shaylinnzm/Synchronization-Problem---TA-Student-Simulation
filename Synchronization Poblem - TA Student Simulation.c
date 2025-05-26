#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h> // For sleep()
#include <errno.h>  // For errno with sem_trywait

#define MAX_CHAIRS 5
#define STUDENT_MIN_ARRIVAL_INTERVAL 0 // Min seconds between student thread creations
#define STUDENT_MAX_ARRIVAL_INTERVAL 2 // Max seconds between student thread creations
#define TA_MIN_HELP_DURATION 1         // Min seconds TA helps a student
#define TA_MAX_HELP_DURATION 3         // Max seconds TA helps a student

// Semaphores
sem_t chairs_sem;        // Counts available chairs, initialized to MAX_CHAIRS
sem_t student_ready_sem; // TA waits on this; student posts when in a chair & ready
sem_t ta_finished_sem;   // Student waits on this; TA posts when done helping

// Mutex for shared data and synchronized printing
pthread_mutex_t shared_data_mutex;

// Shared variables (protected by shared_data_mutex)
int num_waiting_students = 0;
volatile int simulation_active = 1; // Controls TA thread lifecycle

// Function prototypes
void* student_thread_func(void* arg);
void* ta_thread_func(void* arg);

// Helper for random sleep duration
void random_sleep_ms(int min_ms, int max_ms) {
    if (min_ms >= max_ms) {
        usleep(min_ms * 1000);
        return;
    }
    int duration_ms = (rand() % (max_ms - min_ms + 1)) + min_ms;
    usleep(duration_ms * 1000); // usleep takes microseconds
}


int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <num_students>\n", argv[0]);
        return 1;
    }

    int num_students_total = atoi(argv[1]);
    if (num_students_total <= 0) {
        fprintf(stderr, "Number of students must be positive.\n");
        return 1;
    }

    pthread_t ta_tid;
    pthread_t student_tids[num_students_total];
    long student_ids[num_students_total];

    srand(time(NULL)); // Seed random number generator

    // Initialize mutex
    if (pthread_mutex_init(&shared_data_mutex, NULL) != 0) {
        perror("Mutex init failed");
        return 1;
    }

    // Initialize semaphores
    if (sem_init(&chairs_sem, 0, MAX_CHAIRS) != 0) { // 0 for thread-shared, MAX_CHAIRS initial value
        perror("Chairs semaphore init failed");
        return 1;
    }
    if (sem_init(&student_ready_sem, 0, 0) != 0) { // TA sleeps initially
        perror("Student ready semaphore init failed");
        return 1;
    }
    if (sem_init(&ta_finished_sem, 0, 0) != 0) { // Student waits for TA completion
        perror("TA finished semaphore init failed");
        return 1;
    }

    // Create TA thread
    if (pthread_create(&ta_tid, NULL, ta_thread_func, NULL) != 0) {
        perror("Failed to create TA thread");
        return 1;
    }

    // Create student threads
    for (int i = 0; i < num_students_total; ++i) {
        student_ids[i] = i + 1; // Student IDs from 1 to N
        if (pthread_create(&student_tids[i], NULL, student_thread_func, (void*)student_ids[i]) != 0) {
            fprintf(stderr, "Failed to create student thread %d\n", i + 1);
            // In a real scenario, might try to clean up already created threads/TA
            // For this simulation, we'll exit.
            simulation_active = 0; // Signal TA to stop
            sem_post(&student_ready_sem); // Wake up TA if sleeping
            pthread_join(ta_tid, NULL); // Wait for TA to exit
            // ... destroy semaphores/mutex ...
            return 1;
        }
        // Random delay for student "entering the office" at different times
        random_sleep_ms(STUDENT_MIN_ARRIVAL_INTERVAL * 1000, STUDENT_MAX_ARRIVAL_INTERVAL * 1000);
    }

    // Wait for all student threads to complete
    for (int i = 0; i < num_students_total; ++i) {
        pthread_join(student_tids[i], NULL);
    }

    pthread_mutex_lock(&shared_data_mutex);
    printf("\nMAIN: All %d student threads have completed.\n", num_students_total);
    pthread_mutex_unlock(&shared_data_mutex);

    // Signal TA to shut down
    pthread_mutex_lock(&shared_data_mutex);
    simulation_active = 0;
    pthread_mutex_unlock(&shared_data_mutex);
    sem_post(&student_ready_sem); // Wake up TA if it's sleeping to check the flag

    // Wait for TA thread to complete
    pthread_join(ta_tid, NULL);

    pthread_mutex_lock(&shared_data_mutex);
    printf("MAIN: TA thread has completed. Simulation finished.\n");
    pthread_mutex_unlock(&shared_data_mutex);

    // Destroy semaphores and mutex
    sem_destroy(&chairs_sem);
    sem_destroy(&student_ready_sem);
    sem_destroy(&ta_finished_sem);
    pthread_mutex_destroy(&shared_data_mutex);

    return 0;
}

void* student_thread_func(void* arg) {
    long id = (long)arg;

    pthread_mutex_lock(&shared_data_mutex);
    printf("Student %ld: Arrives at the office.\n", id);
    pthread_mutex_unlock(&shared_data_mutex);

    // Try to get a chair
    if (sem_trywait(&chairs_sem) == -1) {
        // EAGAIN means the semaphore count was 0 (no chairs), non-blocking call returned immediately.
        if (errno == EAGAIN) {
            pthread_mutex_lock(&shared_data_mutex);
            printf("Student %ld: Office is full (no chairs available). Leaving.\n", id);
            pthread_mutex_unlock(&shared_data_mutex);
        } else {
            perror("Student: sem_trywait on chairs_sem failed unexpectedly");
        }
        pthread_exit(NULL); // Student leaves
    }

    // Successfully got a chair
    pthread_mutex_lock(&shared_data_mutex);
    num_waiting_students++;
    printf("Student %ld: Took a chair. Students currently waiting in chairs: %d/%d.\n", id, num_waiting_students, MAX_CHAIRS);
    pthread_mutex_unlock(&shared_data_mutex);

    // Signal TA that a student is ready and in a chair
    sem_post(&student_ready_sem);

    pthread_mutex_lock(&shared_data_mutex);
    printf("Student %ld: Is now waiting for the TA.\n", id);
    pthread_mutex_unlock(&shared_data_mutex);

    // Wait for TA to finish helping this specific student
    sem_wait(&ta_finished_sem);

    pthread_mutex_lock(&shared_data_mutex);
    printf("Student %ld: Received help from TA and is now leaving the office.\n", id);
    pthread_mutex_unlock(&shared_data_mutex);
    
    pthread_exit(NULL);
}

void* ta_thread_func(void* arg) {
    pthread_mutex_lock(&shared_data_mutex);
    printf("TA: Office is open. TA is initially resting.\n");
    pthread_mutex_unlock(&shared_data_mutex);

    while (1) {
        // Wait for a student to be ready (TA sleeps here if student_ready_sem is 0)
        // This semaphore is also used to wake up the TA for shutdown.
        if (sem_wait(&student_ready_sem) == -1) {
            // This could happen if sem_destroy is called while TA is waiting,
            // though our shutdown logic should prevent this.
            perror("TA: sem_wait on student_ready_sem failed");
            break;
        }

        // TA is awakened. Check if it's for shutdown or a student.
        pthread_mutex_lock(&shared_data_mutex);
        if (!simulation_active && num_waiting_students == 0) {
            // Shutdown signal received, and no students are actually waiting in chairs.
            printf("TA: Shutdown signal received and no students to help. Closing office.\n");
            pthread_mutex_unlock(&shared_data_mutex);
            break; // Exit TA loop
        }
        // If we're here, either simulation is active, or it's inactive but there's a student.
        // A student is ready to be moved from a chair to the TA.
        num_waiting_students--;
        printf("TA: Woke up / Is ready for next student. Students now waiting in chairs: %d.\n", num_waiting_students);
        pthread_mutex_unlock(&shared_data_mutex);

        // The chair previously occupied by the student being helped is now free.
        sem_post(&chairs_sem);

        pthread_mutex_lock(&shared_data_mutex);
        printf("TA: Helping a student...\n");
        pthread_mutex_unlock(&shared_data_mutex);

        random_sleep_ms(TA_MIN_HELP_DURATION * 1000, TA_MAX_HELP_DURATION * 1000); // Simulate helping

        pthread_mutex_lock(&shared_data_mutex);
        printf("TA: Finished helping the student.\n");
        pthread_mutex_unlock(&shared_data_mutex);

        // Signal the student (who was waiting on ta_finished_sem) that help is complete.
        sem_post(&ta_finished_sem);

        // After helping, check if TA should announce resting.
        pthread_mutex_lock(&shared_data_mutex);
        if (num_waiting_students == 0 && simulation_active) {
            printf("TA: No students currently waiting in chairs. TA is resting.\n");
        }
        pthread_mutex_unlock(&shared_data_mutex);
    }
    pthread_exit(NULL);
}