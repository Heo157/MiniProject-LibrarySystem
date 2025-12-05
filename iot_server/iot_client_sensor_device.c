#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>
#include <signal.h>
#include <mysql/mysql.h>

#define BUF_SIZE 1024
#define NAME_SIZE 20
#define ARR_CNT 5

// --- 함수 프로토타입 (수정됨) ---
void* barcode_handler(void* arg);
void* recv_msg(void* arg);
void error_handling(char* msg);
// 'sender_id' 인자 추가
void process_checkout(int sock, const char* sender_id, const char* user_id, const char* book_id);
void process_return(int sock, const char* sender_id, const char* user_id, const char* book_id);

// --- 전역 변수 (수정됨) ---
char name[NAME_SIZE] = "[Default]";
char msg[BUF_SIZE];
char currentUser[NAME_SIZE] = { 0 };
char currentMode[NAME_SIZE] = { 0 };
char currentSender[NAME_SIZE] = { 0 }; 
MYSQL* conn;
pthread_mutex_t mutx;

int main(int argc, char* argv[]) {
    // main 함수는 변경사항 없음
    int sock;
    struct sockaddr_in serv_addr;
    pthread_t snd_thread, rcv_thread;
    void* thread_return;

    if (argc != 4) {
        printf("Usage : %s <IP> <port> <name>\n", argv[0]);
        exit(1);
    }

    char* host = "localhost";
    char* user = "library";
    char* pass = "pwlibrary";
    char* dbname = "library_db";
    conn = mysql_init(NULL);
    if (!(mysql_real_connect(conn, host, user, pass, dbname, 0, NULL, 0))) {
        fprintf(stderr, "ERROR : %s[%d]\n", mysql_error(conn), mysql_errno(conn));
        exit(1);
    }
    else {
        printf("MySQL Connection Successful!\n\n");
    }

    pthread_mutex_init(&mutx, NULL);

    sprintf(name, "%s", argv[3]);
    sock = socket(PF_INET, SOCK_STREAM, 0);
    if (sock == -1) error_handling("socket() error");

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = inet_addr(argv[1]);
    serv_addr.sin_port = htons(atoi(argv[2]));

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == -1)
        error_handling("connect() error");

    sprintf(msg, "[%s:PASSWD]", name);
    write(sock, msg, strlen(msg));

    pthread_create(&rcv_thread, NULL, recv_msg, (void*)&sock);
    pthread_create(&snd_thread, NULL, barcode_handler, (void*)&sock);

    pthread_join(snd_thread, &thread_return);
    pthread_join(rcv_thread, &thread_return);

    mysql_close(conn);
    close(sock);
    pthread_mutex_destroy(&mutx);
    return 0;
}

void* barcode_handler(void* arg) {
    int sock = *(int*)arg;
    char book_id_buf[BUF_SIZE];
    char user_id_buf[NAME_SIZE];
    char mode_buf[NAME_SIZE];
    char sender_id_buf[NAME_SIZE]; 

    puts("Barcode scanner is ready...");
    while (fgets(book_id_buf, BUF_SIZE, stdin) != NULL) {
        book_id_buf[strcspn(book_id_buf, "\n")] = 0;

        if (strlen(book_id_buf) == 0) {
            continue;
        }
        printf("Barcode scanned: %s\n", book_id_buf);

        pthread_mutex_lock(&mutx);
        strcpy(user_id_buf, currentUser);
        strcpy(mode_buf, currentMode);
        strcpy(sender_id_buf, currentSender); 
        pthread_mutex_unlock(&mutx);

        if (strlen(user_id_buf) == 0) {
            printf("Error: User is not tagged.\n");
            snprintf(msg, BUF_SIZE, "[%s]Please tag RFID.\n", sender_id_buf);
            write(sock, msg, strlen(msg));
            continue;
        }
        if (strlen(mode_buf) == 0) {
            printf("Error: Mode is not selected.\n");
            snprintf(msg, BUF_SIZE, "[%s]Please select mode.\n", sender_id_buf);
            write(sock, msg, strlen(msg));
            continue;
        }

        if (strcmp(mode_buf, "CHECKOUT") == 0) {
            process_checkout(sock, sender_id_buf, user_id_buf, book_id_buf);

        }
        else if (strcmp(mode_buf, "RETURN") == 0) {
            process_return(sock, sender_id_buf, user_id_buf, book_id_buf);
        }

    }
    return NULL;
}

// iot_client_db.c 파일의 recv_msg 함수를 아래 코드로 교체하세요.

void* recv_msg(void* arg) {
    int sock = *(int*)arg;
    char name_msg[NAME_SIZE + BUF_SIZE + 1];
    char* pToken;
    char* pArray[ARR_CNT] = { 0 };
    int i;
    char response_msg[BUF_SIZE];

    while (1) {
        int str_len = read(sock, name_msg, NAME_SIZE + BUF_SIZE);
        if (str_len <= 0) break;

        name_msg[str_len] = '\0';
        fputs(name_msg, stdout);

        char temp_msg[NAME_SIZE + BUF_SIZE + 1];
        strcpy(temp_msg, name_msg);

        pToken = strtok(temp_msg, "[:@\n]");
        i = 0;
        while (pToken != NULL && i < ARR_CNT) {
            pArray[i++] = pToken;
            pToken = strtok(NULL, "[:@\n]");
        }

        if (i < 2) continue; // 보낸사람, 명령어 최소 2개

        char* sender = pArray[0];
        char* command = pArray[1];
        char* value = (i > 2) ? pArray[2] : "";

        if (strlen(sender) > 0) {
            pthread_mutex_lock(&mutx);
            if (strcmp(command, "CHECKOUT") == 0) {
                strcpy(currentSender, sender);
                strcpy(currentMode, "CHECKOUT");
                strcpy(currentUser, value);
                printf("System State Updated: Sender='%s', User='%s', Mode='CHECKOUT'\n", currentSender, currentUser);
            }
            else if (strcmp(command, "RETURN") == 0) {
                strcpy(currentSender, sender);
                strcpy(currentMode, "RETURN");
                strcpy(currentUser, value);
                printf("System State Updated: Sender='%s', User='%s', Mode='RETURN'\n", currentSender, currentUser);
            }
            else if (strcmp(command, "SCANEND") == 0) {
                sleep(1);
                sprintf(response_msg, "[%s]%s@OK\n", sender, currentMode);
                write(sock, response_msg, strlen(response_msg));
                sleep(1);
                if (strcmp(currentMode, "RETURN") == 0) {
                    char open_cmd[] = "[LIB_STM]DOOR@OPEN\n";
                    write(sock, open_cmd, strlen(open_cmd));
                    sleep(5);
                    char close_cmd[] = "[LIB_STM]DOOR@CLOSE\n";
                    write(sock, close_cmd, strlen(close_cmd));
                }
                
                // 현재 사용자, 모드, 보낸사람 정보를 모두 초기화
                currentUser[0] = '\0';
                currentMode[0] = '\0';
                currentSender[0] = '\0';
                //printf("System State Cleared: Session ended by %s.\n", sender);
            }            

            pthread_mutex_unlock(&mutx);
        }
    }
    return NULL;
}

// iot_client_db.c 파일의 process_checkout 함수를 아래 코드로 교체하세요.

void process_checkout(int sock, const char* sender_id, const char* user_id, const char* book_id) {
    char sql_cmd[256];
    char response_msg[BUF_SIZE];


    // ★★ 1. 사용자의 현재 대출 권수 및 연체 상태 확인 ★★
    snprintf(sql_cmd, sizeof(sql_cmd), "SELECT borrowed_count, borrow_ban_until FROM users WHERE user_id='%s'", user_id);
    if (mysql_query(conn, sql_cmd)) {
        fprintf(stderr, "SELECT user error: %s\n", mysql_error(conn));
        return;
    }
    MYSQL_RES* result_user = mysql_store_result(conn);
    if (mysql_num_rows(result_user) == 0) {
        mysql_free_result(result_user);
        return;
    }

    MYSQL_ROW row_user = mysql_fetch_row(result_user);
    int borrowed_count = atoi(row_user[0]);
    char* ban_until = row_user[1];

    // 1-1. 대출 권수 확인
    if (borrowed_count >= 5) {
        snprintf(response_msg, sizeof(response_msg), "[%s]CHECKOUT@DENY@LIMIT\n", sender_id);
        write(sock, response_msg, strlen(response_msg));
        mysql_free_result(result_user);
        return;
    }

    // 1-2. 연체 상태 확인
    if (ban_until != NULL && strcmp(ban_until, "0000-00-00") > 0) {
        snprintf(sql_cmd, sizeof(sql_cmd), "SELECT CURDATE() <= '%s'", ban_until);
        mysql_query(conn, sql_cmd);
        MYSQL_RES* res_date = mysql_store_result(conn);
        MYSQL_ROW row_date = mysql_fetch_row(res_date);
        if (atoi(row_date[0]) == 1) {
            snprintf(response_msg, sizeof(response_msg), "[%s]FAIL@Banned until %s.\n", sender_id, ban_until);
            write(sock, response_msg, strlen(response_msg));
            mysql_free_result(result_user);
            mysql_free_result(res_date);
            return;
        }
        mysql_free_result(res_date);
    }
    mysql_free_result(result_user);


    // 2. 대출 처리 (Transaction)
    mysql_query(conn, "START TRANSACTION");
    sprintf(sql_cmd, "SELECT status, loan_period FROM books WHERE book_id='%s' FOR UPDATE", book_id);
    if (mysql_query(conn, sql_cmd)) {
        fprintf(stderr, "SELECT book error: %s\n", mysql_error(conn));
        mysql_query(conn, "ROLLBACK");
        return;
    }
    MYSQL_RES* result_book = mysql_store_result(conn);
    if (mysql_num_rows(result_book) == 0) {
        snprintf(response_msg, sizeof(response_msg), "[%s]FAIL@Book does not exist.\n", sender_id);
    }
    else {
        MYSQL_ROW row_book = mysql_fetch_row(result_book);
        if (strcmp(row_book[0], "available") != 0) {
            snprintf(response_msg, sizeof(response_msg), "[%s]FAIL@Already checked out.\n", sender_id);
        }
        else {
            //2-1. users 테이블의 대출 권수 1 증가
            sprintf(sql_cmd, "UPDATE users SET borrowed_count = borrowed_count + 1 WHERE user_id='%s'", user_id);
            if (mysql_query(conn, sql_cmd)) {
                fprintf(stderr, "UPDATE users count error: %s\n", mysql_error(conn));
                mysql_query(conn, "ROLLBACK");
                mysql_free_result(result_book);
                return;
            }

            // 2-2. books 테이블 상태 변경 및 logs 기록
            int loan_period = atoi(row_book[1]);
            sprintf(sql_cmd, "UPDATE books SET status='checked_out' WHERE book_id='%s'", book_id);
            if (mysql_query(conn, sql_cmd)) {
                fprintf(stderr, "UPDATE books error: %s\n", mysql_error(conn));
                mysql_query(conn, "ROLLBACK");
                mysql_free_result(result_book);
                return;
            }
            sprintf(sql_cmd, "INSERT INTO logs (book_id, user_id, checkout_date, due_date) VALUES ('%s', '%s', NOW(), CURDATE() + INTERVAL %d DAY)", book_id, user_id, loan_period);
            if (mysql_query(conn, sql_cmd)) {
                fprintf(stderr, "INSERT logs error: %s\n", mysql_error(conn));
                mysql_query(conn, "ROLLBACK");
                mysql_free_result(result_book);
                return;
            }
            snprintf(response_msg, sizeof(response_msg), "[%s]CHECKOUT@OK\n", sender_id);
        }
    }

    write(sock, response_msg, strlen(response_msg));
    mysql_free_result(result_book);
    mysql_query(conn, "COMMIT");
}


// iot_client_db.c 파일의 process_return 함수를 아래 코드로 교체하세요.

void process_return(int sock, const char* sender_id, const char* user_id, const char* book_id) {
    char sql_cmd[256];
    char response_msg[BUF_SIZE];

    mysql_query(conn, "START TRANSACTION");
    sprintf(sql_cmd, "SELECT due_date FROM logs WHERE book_id='%s' AND user_id='%s' AND return_date IS NULL FOR UPDATE", book_id, user_id);
    if (mysql_query(conn, sql_cmd)) {
        fprintf(stderr, "SELECT logs error: %s\n", mysql_error(conn));
        mysql_query(conn, "ROLLBACK");
        return;
    }
    MYSQL_RES* result_return = mysql_store_result(conn);
    if (mysql_num_rows(result_return) == 0) {
        snprintf(response_msg, sizeof(response_msg), "[%s]FAIL@Not checked out by you.\n", sender_id);
        write(sock, response_msg, strlen(response_msg));
        mysql_free_result(result_return);
        mysql_query(conn, "ROLLBACK");
        return;
    }

    // ★★ 1. 반납 성공 시 users 테이블의 대출 권수 1 감소 ★★
    sprintf(sql_cmd, "UPDATE users SET borrowed_count = borrowed_count - 1 WHERE user_id='%s' AND borrowed_count > 0", user_id);
    if (mysql_query(conn, sql_cmd)) {
        fprintf(stderr, "UPDATE users count error: %s\n", mysql_error(conn));
        mysql_query(conn, "ROLLBACK");
        mysql_free_result(result_return);
        return;
    }

    // 2. 연체일 계산 및 페널티 부여
    MYSQL_ROW row_return = mysql_fetch_row(result_return);
    sprintf(sql_cmd, "SELECT DATEDIFF(CURDATE(), '%s')", row_return[0]);
    mysql_free_result(result_return);
    if (mysql_query(conn, sql_cmd)) {
        fprintf(stderr, "SELECT DATEDIFF error: %s\n", mysql_error(conn));
        mysql_query(conn, "ROLLBACK");
        return;
    }
    MYSQL_RES* result_diff = mysql_store_result(conn);
    MYSQL_ROW row_diff = mysql_fetch_row(result_diff);
    int overdue_days = atoi(row_diff[0]);
    mysql_free_result(result_diff);

    if (overdue_days > 0) {
        sprintf(sql_cmd, "UPDATE users SET borrow_ban_until=CURDATE() + INTERVAL %d DAY WHERE user_id='%s'", overdue_days, user_id);
        mysql_query(conn, sql_cmd);
        snprintf(response_msg, sizeof(response_msg), "[%s]RETURN@DENY@OVERDUE@DAYS=%d\n", sender_id, overdue_days);
        write(sock, response_msg, strlen(response_msg));
        sleep(2);
    }


    // 3. books 테이블 상태 변경 및 logs 기록
    sprintf(sql_cmd, "UPDATE books SET status='available' WHERE book_id='%s'", book_id);
    mysql_query(conn, sql_cmd);
    sprintf(sql_cmd, "UPDATE logs SET return_date=NOW() WHERE book_id='%s' AND user_id ='%s' AND return_date IS NULL", book_id, user_id);
    mysql_query(conn, sql_cmd);


    snprintf(response_msg, sizeof(response_msg), "[%s]RETURN@OK\n", sender_id);
    write(sock, response_msg, strlen(response_msg));

    mysql_query(conn, "COMMIT");
}

void error_handling(char* msg) {
    fputs(msg, stderr);
    fputc('\n', stderr);
    exit(1);
} 