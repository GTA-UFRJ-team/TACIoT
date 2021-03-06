/*
 * Teleinformatic and Automation Group (GTA, Coppe, UFRJ)
 * Author: Guilherme Araujo Thomaz
 * Descripton: query message and return for client
 */

#include <cstdio>
#include <stdlib.h>
#include <iostream>
#include <string.h>
#include <stdio.h>
#include <chrono>
#include <thread>
#include "timer.h"

#include "server_query.h"
#include "sample_libcrypto.h"   // sample_aes_gcm_128bit_key_t
#include "config_macros.h"      // ULTRALIGH_SAMPLE
#include "utils_sgx.h"
#include "utils.h"
#include "server_enclave_u.h"
#include "ecp.h"                // sample_ec_key_128bit_t
#include HTTPLIB_PATH

#include "sgx_urts.h"
#include "sgx_eid.h"
#include "sgx_ukey_exchange.h"
#include "sgx_uae_epid.h"
#include "sgx_uae_quote_ex.h"
#include "sgx_tcrypto.h"

using namespace httplib;

uint32_t parse_query(uint32_t size, char* msg, char* pk)
{
    Timer t("parse_query");
    uint32_t index;
    char* token = strtok_r(msg, "|", &msg);
    int i = 0;
    while (token != NULL)
    {
        i++;
        token = strtok_r(NULL, "|", &msg);
        // Get client key
        if (i == 1)
        {
            memcpy(pk, token, 8);
            pk[8] = '\0';
        }
        // Get data index
        if (i == 3)
            index = (uint32_t)strtoul(token, NULL, 10);
        
        //printf("%s\n", token);
    }
    return index;
}

stored_data_t get_stored_parameters(char* msg)
{
    Timer t("get_stored_parameters");
    // type|%s|pk|%s|size|0x%02x|encrypted|
    stored_data_t stored;
    uint32_t index;
    char* token = strtok_r(msg, "|", &msg);
    int i = 0;
    char auxiliar[3];
    while (token != NULL && i<6)
    {
        i++;
        token = strtok_r(NULL, "|", &msg);
        // Get type     
        if (i == 1)
        {
            memcpy(stored.type, token, 6);
            stored.type[6] = '\0';
        }
        // Get client key       
        if (i == 3)
        {
            memcpy(stored.pk, token, 8);
            stored.pk[8] = '\0';
        }
        // Get encrypted message size
        if (i == 5)
        {
            stored.encrypted_size = (uint32_t)strtoul(token,NULL,16);
            //printf("%u\n", stored.encrypted_size);
        }
    }

    // Get encrypted
    stored.encrypted = (uint8_t*)malloc((stored.encrypted_size+1) * sizeof(uint8_t));
    if (stored.encrypted == NULL)
        printf("Allocation error\n");
    memcpy(stored.encrypted, msg, stored.encrypted_size);
    stored.encrypted[stored.encrypted_size] = 0;
    //debug_print_encrypted((size_t)(stored.encrypted_size), stored.encrypted);

    return stored;
}

void file_read(uint32_t index, char* data)
{
    Timer t("file_read");
    char db_path[DB_PATH_SIZE];
    sprintf(db_path, "%s", DB_PATH);
    FILE* db_file = fopen(db_path, "rb");
    if (db_file == NULL)
    {
        printf("Failed opening %s file", db_path);
    }
    fseek(db_file, (long)index, 0);
    char published_header[45];
    fread(published_header,1,44,db_file);
    published_header[44] = 0;
    memcpy(&data[0], published_header, 44);

    //printf("%s\n", published_header);

    char auxiliar[3];
    auxiliar[0] = published_header[31];
    auxiliar[1] = published_header[32];
    auxiliar[2] = '\0';
    uint32_t encrypted_size = (uint32_t)strtoul(auxiliar, NULL, 16);

    char *encrypted = (char*)malloc(6*encrypted_size+1);
    fread(encrypted,1,encrypted_size*6,db_file);
    for (uint32_t i=0; i<encrypted_size; i++){
        auxiliar[0] = encrypted[6*i+2];
        auxiliar[1] = encrypted[6*i+3];
        auxiliar[2] = '\0';
        data[44+i] = (char)strtoul(auxiliar, NULL, 16);
    }
    data[44+encrypted_size] = '\0';
    fclose(db_file);
    free(encrypted);
/*
    uint8_t* data_int = (uint8_t*)malloc(44+encrypted_size);
    memcpy(data_int, data, (size_t)(44+encrypted_size));
    debug_print_encrypted(44+(size_t)encrypted_size,data_int);
    free(data_int);
    */
}

uint32_t get_query_message(const Request& req, char* snd_msg)
{
    Timer t("get_query_message");
    char c_size[4];
    uint32_t size;
    std::string a_size = req.matches[1].str();
    strcpy(c_size, a_size.c_str());
    size = (uint32_t)strtoul(c_size, NULL, 10);

    std::string a_snd_msg = req.matches[2].str();
    strncpy(snd_msg, a_snd_msg.c_str(), (size_t)(size-1));
    snd_msg[size] = '\0';

    //printf("Size=%u, Message=%s\n", (unsigned)size, snd_msg);

    return size;
}

uint8_t enclave_get_response(stored_data_t stored, sgx_enclave_id_t global_eid, uint8_t* response, char* querier_pk)
{
    Timer t("enclave_get_response");
    // Get querier sealed key
    char seal_path[PATH_MAX_SIZE];
    sprintf(seal_path, "%s/%s", SEALS_PATH, querier_pk);
    FILE* seal_file = fopen(seal_path, "rb");
    size_t sealed_size = sizeof(sgx_sealed_data_t) + sizeof(uint8_t)*16;
    uint8_t* sealed_querier_key = (uint8_t*)malloc(sealed_size);
    if (seal_file == NULL) {
        printf("\nWarning: Failed to open the seal file \"%s\".\n", seal_path);
        fclose(seal_file);
        free(sealed_querier_key);
        return 1;
    }
    else {
        fread(sealed_querier_key,1,sealed_size,seal_file);
        fclose(seal_file);
    }

    // Get publisher sealed key
    sprintf(seal_path, "%s/%s", SEALS_PATH, stored.pk);
    seal_file = fopen(seal_path, "rb");
    uint8_t* sealed_publisher_key = (uint8_t*)malloc(sealed_size);
    if (seal_file == NULL) {
        printf("\nWarning: Failed to open the seal file \"%s\".\n", seal_path);
        fclose(seal_file);
        free(sealed_publisher_key);
        return 1;
    }
    else {
        fread(sealed_publisher_key,1,sealed_size,seal_file);
        fclose(seal_file);
    }

    //printf("%s\n", querier_pk);

    // Call enclave to unseal keys, decrypt with the publisher key and encrypt with querier key
    sgx_status_t ecall_status;
    sgx_status_t sgx_status;
    uint32_t real_size;
    uint8_t access_allowed = 0;
    sgx_status = retrieve_data(global_eid, &ecall_status,
        (sgx_sealed_data_t*)sealed_querier_key,
        (sgx_sealed_data_t*)sealed_publisher_key,
        stored.encrypted,
        stored.encrypted_size,
        querier_pk,
        response,
        &access_allowed);
    free(sealed_querier_key);
    free(sealed_publisher_key);
    return access_allowed;
}

void make_response(uint8_t* enc_data, uint32_t enc_data_size, char* response)
{
    Timer t("make_response");
    sprintf(response, "size|0x%02x|data|", enc_data_size);
    char auxiliar[7];
    for (uint32_t count=0; count<enc_data_size; count++)
    {
        sprintf(auxiliar, "0x%02x--", enc_data[count]);
        memcpy(&response[15+count*6], auxiliar, 6);
    }
    response[15+enc_data_size*6] = '\0';
}

int server_query(bool secure, const Request& req, Response& res, sgx_enclave_id_t global_eid)
{
    Timer t("server_query");
    // Get message sent in HTTP header
    char* snd_msg = (char*)malloc(URL_MAX_SIZE*sizeof(char));
    uint32_t size = get_query_message(req, snd_msg);

    // Get data index and pk
    char pk[9];
    uint32_t disk_index = parse_query(size, snd_msg, pk);
    free(snd_msg);

    // Read data from BD/disk copy
    char* data = (char *)malloc(MAX_DATA_SIZE*sizeof(char));
    file_read(disk_index, data);

    // Separate parameters of stored data
    stored_data_t message = get_stored_parameters(data);
    free(data);

    uint8_t *enc_data = (uint8_t*)malloc(message.encrypted_size*sizeof(char));
    char *response = (char*)malloc((15+6*message.encrypted_size+1)*sizeof(char));
    if (secure == true)
    {
        uint8_t access_allowed = enclave_get_response(message, global_eid, enc_data, pk);
        if (!access_allowed)
            res.set_content("Denied", "text/plain");
        else {
            make_response(enc_data, message.encrypted_size, response);
            res.set_content(response, "text/plain");
        }
    } else{
        make_response(message.encrypted, message.encrypted_size, response);
        res.set_content(response, "text/plain");
    }
    //printf("%s\n", response);
    free(response);
    free(enc_data);
    free(message.encrypted);
    return 0; 
}