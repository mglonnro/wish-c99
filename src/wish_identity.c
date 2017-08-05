#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "wish_io.h"
#include "wish_identity.h"
#include "ed25519.h"
#include "mbedtls/sha256.h"
#include "wish_debug.h"
#include "wish_fs.h"
#include "bson_visit.h"
#include "wish_port_config.h"
#include "wish_connection_mgr.h"

#include "utlist.h"

int wish_save_identity_entry(wish_identity_t *identity) {
    int num_uids_in_db = wish_get_num_uid_entries();
    wish_uid_list_elem_t uid_list[num_uids_in_db];
    int num_uids = wish_load_uid_list(uid_list, num_uids_in_db);

    if(num_uids >= WISH_PORT_MAX_UIDS) {
        // DB is full, return error
        WISHDEBUG(LOG_CRITICAL, "Too many identities in database");
        return -1;
    }
    
    const uint32_t identity_doc_max_len = sizeof (wish_identity_t) + 100;
    uint8_t identity_doc[identity_doc_max_len];
    /* Create the new BSON document in memory */
    
    bson bs;
    bson_init_buffer(&bs, identity_doc, identity_doc_max_len);
    
    bson_append_string(&bs, "alias", identity->alias);
    bson_append_binary(&bs, "uid", identity->uid, WISH_ID_LEN);
    bson_append_binary(&bs, "pubkey", identity->pubkey, WISH_PUBKEY_LEN);

    if (identity->has_privkey) {
        bson_append_binary(&bs, "privkey", identity->privkey, WISH_PRIVKEY_LEN);
    }

    //WISHDEBUG(LOG_CRITICAL, "transports[0] to save: %s", identity->transports[0]);
    /** \fixme For now, just encode a single transport */
    if (strnlen(&(identity->transports[0][0]), WISH_MAX_TRANSPORT_LEN) > 0) {
        bson_append_start_array(&bs, "transports");
        bson_append_string(&bs, "0", &(identity->transports[0][0]));
        bson_append_finish_array(&bs);
    }
    
    bson_finish(&bs);
    //bson_visit("Identity BSON to be saved:", bson_data(&bs));'
    
    /* FIXME add the rest of the fields */

    int ret = wish_save_identity_entry_bson(bson_data(&bs));
    if (ret == 0) {
        return -2;
    } else {
        return 0;
    }
}

/**
 * Save identity, expressed in BSON format, to the identity database 
 * 
 * Returns 0 on error, and bytes written on success
 * 
 * @param identity
 * @return 
 */
int wish_save_identity_entry_bson(const uint8_t* identity) {
    wish_file_t fd;
    int32_t io_retval = 0;
    fd = wish_fs_open(WISH_ID_DB_NAME);
    if (fd < 0) {
        /* error */
        WISHDEBUG(LOG_CRITICAL, "could not open identity db");
        return 0;
    }

    /* FIXME Find if there is already an existing entry for this identity. If
     * there this, delete the old entry */

    /* APPEEND the new BSON document to stable storage file -  */
    io_retval = wish_fs_lseek(fd, 0, WISH_FS_SEEK_END);
    if (io_retval < 0) {
        /* error */
        WISHDEBUG(LOG_CRITICAL, "error seeking");
        return 0;
    }

    bson bs;
    bson_init_with_data(&bs, identity);
    
    io_retval = wish_fs_write(fd, bson_data(&bs), bson_size(&bs));
    if (io_retval <= 0) {
        /* error */
        WISHDEBUG(LOG_CRITICAL, "error writing");
        return 0;
    }
    wish_fs_close(fd);

    return io_retval;
}

/** This function returns the number of entries in the identity database (number of true identities + contacts),
 * Returns the number of identities, or -1 for errors */
int wish_get_num_uid_entries(void) {
    int retval = 0;
    wish_file_t fd = wish_fs_open(WISH_ID_DB_NAME);
    wish_offset_t prev_offset = 0;

    int num_ids = 0;
    
    while (num_ids <= WISH_PORT_MAX_UIDS) {
        /* Determine length and uid of next element */
        int peek_len = sizeof (wish_identity_t) + 100;
        uint8_t peek_buf[peek_len];

        WISHDEBUG(LOG_DEBUG, "Seeking to offset %d", prev_offset);
        int io_retval = wish_fs_lseek(fd, prev_offset, WISH_FS_SEEK_SET);
        if (io_retval == -1) {
            WISHDEBUG(LOG_CRITICAL, "Error seeking");
            retval = -1;
            break;
        }
        io_retval = wish_fs_read(fd, peek_buf, peek_len);
        if (io_retval == 0) {
            WISHDEBUG(LOG_DEBUG, "End of file detected");
            /* Success exit */
            retval = num_ids;
            break;
        }
        else if (io_retval < 0) {
            WISHDEBUG(LOG_CRITICAL, "read error");
            retval = -1;
            break;
        }

        bson bs;
        
        bson_init_with_data(&bs, peek_buf);
        
        int32_t elem_len = bson_size(&bs);
        if (elem_len < 4 || elem_len > peek_len) {
            WISHDEBUG(LOG_CRITICAL, "BSON Read error");
            retval =  -1;
            break;
        }
        /* Update prev offset so that we can later re-position the
         * stream */
        prev_offset+=elem_len;

        bson_iterator it;
        
        if ( bson_find_from_buffer(&it, peek_buf, "uid") != BSON_BINDATA ) {
            WISHDEBUG(LOG_CRITICAL, "Could not get uid (a)");
            retval = -1;
            break;
        }
        
        WISHDEBUG(LOG_DEBUG, "Found identity!");
        num_ids++;
        
    }
    
    if (num_ids > WISH_PORT_MAX_UIDS) {
        WISHDEBUG(LOG_CRITICAL, "Number of identities in db exceeds allowable number of identities (%d)!", WISH_PORT_MAX_UIDS);
        retval = WISH_PORT_MAX_UIDS;
    }
    
    wish_fs_close(fd);
    return retval;
}


/**
 * This function returns the list of UIDs which are in the identity
 * database. A pointer to the list of UIDs are stored to the pointer given as
 * argument. 
 * Returns the number of uids in the list, or 0 if there are no
 * identities in the database, and a negative number for an error */
int wish_load_uid_list(wish_uid_list_elem_t *list, int list_len ) {

    if (list == NULL || list_len == 0) {
        return -1;
    }

    wish_file_t fd = wish_fs_open(WISH_ID_DB_NAME);
    wish_offset_t prev_offset = 0;

    int i = 0;
    for (i = 0; i < list_len; i++) {
        /* Determine length and uid of next element */
        int peek_len = sizeof (wish_identity_t) + 100;
        uint8_t peek_buf[peek_len];

        WISHDEBUG(LOG_DEBUG, "Seeking to offset %d", prev_offset);
        int io_retval = wish_fs_lseek(fd, prev_offset, WISH_FS_SEEK_SET);
        if (io_retval == -1) {
            WISHDEBUG(LOG_CRITICAL, "Error seeking");

        }
        io_retval = wish_fs_read(fd, peek_buf, peek_len);
        if (io_retval == 0) {
            WISHDEBUG(LOG_DEBUG, "End of file detected");
            break;
        }
        else if (io_retval < 0) {
            WISHDEBUG(LOG_CRITICAL, "read error");
            return -1;
        }

        
        bson bs;
        bson_init_with_data(&bs, peek_buf);
        
        int32_t elem_len = bson_size(&bs);
        if (elem_len < 4 || elem_len > peek_len) {
            WISHDEBUG(LOG_CRITICAL, "BSON Read error");
            return -1;
        }
        /* Update prev offset so that we can later re-position the
         * stream */
        prev_offset+=elem_len;

        bson_iterator it;
        
        if (bson_find_from_buffer(&it, peek_buf, "uid") != BSON_BINDATA) {
            WISHDEBUG(LOG_CRITICAL, "Could not get uid (b)");
            return -1;
        }

        const uint8_t* uid = bson_iterator_bin_data(&it);
        int32_t uid_len = bson_iterator_bin_len(&it);
        
        if (uid_len != WISH_UID_LEN) {
            WISHDEBUG(LOG_CRITICAL, "Could not get uid (c)");
            return -1;
        }
        
        WISHDEBUG(LOG_DEBUG, "Found identity!");
        /* Add element to uid list */
        memcpy(list[i].uid, uid, WISH_ID_LEN);
    } 
    wish_fs_close(fd);
    return i;
}


return_t wish_identity_load(const uint8_t *uid, wish_identity_t *identity) {
    int retval = RET_FAIL;

    if (uid == NULL) {
        return retval;
    }

    wish_file_t fd = wish_fs_open(WISH_ID_DB_NAME);
    wish_offset_t prev_offset = 0;

    do {
        /* Determine length and uid of next element */
        int peek_len = sizeof (wish_identity_t) + 100;
        uint8_t peek_buf[peek_len];
        /* Re-position the stream to the end of the previous BSON structure - 
         * so that the next bytes to be read will be of the next element
         * */
        int io_retval = wish_fs_lseek(fd, prev_offset, WISH_FS_SEEK_SET);
        if (io_retval == -1) {
            WISHDEBUG(LOG_CRITICAL, "Error seeking");
            break;

        }
        io_retval = wish_fs_read(fd, peek_buf, peek_len);
        if (io_retval == 0) {
            WISHDEBUG(LOG_DEBUG, "End of file detected (2)");
            break;
        }
        else if (io_retval < 0) {
            WISHDEBUG(LOG_CRITICAL, "read error");
            break;
        }

        bson bs;
        bson_init_with_data(&bs, peek_buf);
        
        int32_t elem_len = bson_size(&bs);
        if (elem_len < 4 || elem_len > peek_len) {
            WISHDEBUG(LOG_CRITICAL, "BSON Read error");
            break;
        }
        /* Update prev offset so that we can later re-position the
         * stream */
        prev_offset+=elem_len;

        bson_iterator it;
        
        if (bson_find_from_buffer(&it, peek_buf, "uid") != BSON_BINDATA) {
            WISHDEBUG(LOG_CRITICAL, "Could not get uid (d)");
            break;
        }

        const uint8_t* peek_uid = bson_iterator_bin_data(&it);
        
        if ( bson_iterator_bin_len(&it) != WISH_UID_LEN ) {
            WISHDEBUG(LOG_CRITICAL, "Invalid uid.");
            break;
        }
        
        if (memcmp(peek_uid, uid, WISH_ID_LEN) == 0) {
            WISHDEBUG(LOG_DEBUG, "Found identity (2)!");
            memcpy(&(identity->uid), peek_uid, WISH_ID_LEN);

            
            if (bson_find_from_buffer(&it, peek_buf, "pubkey") != BSON_BINDATA) {
                WISHDEBUG(LOG_CRITICAL, "Could not load pubkey");
                break;
            }
            
            const uint8_t* pubkey = bson_iterator_bin_data(&it);
            int32_t len = 0;
            
            memcpy(&(identity->pubkey), pubkey, WISH_PUBKEY_LEN);
 
            if (bson_find_from_buffer(&it, peek_buf, "privkey") != BSON_BINDATA) {
                WISHDEBUG(LOG_DEBUG, "No privkey for this identity");
                identity->has_privkey = false;
            } else {
                WISHDEBUG(LOG_DEBUG, "Found privkey for identity");
                if (bson_iterator_bin_len(&it) != WISH_PRIVKEY_LEN) {
                    WISHDEBUG(LOG_CRITICAL, "Could not load privkey, invalid len");
                    break;
                }
                memcpy(&(identity->privkey), bson_iterator_bin_data(&it), WISH_PRIVKEY_LEN);
                identity->has_privkey = true;
            }

            if (bson_find_from_buffer(&it, peek_buf, "alias") != BSON_STRING) {
                WISHDEBUG(LOG_CRITICAL, "Could not get alias");
                break;
            }
            
            const char* alias = bson_iterator_string(&it);
            
            strncpy(&(identity->alias[0]), alias, WISH_MAX_ALIAS_LEN);

            /* When we got this far, we are satisfied with import, the
             * rest is optional */
            retval = RET_SUCCESS;

            bson_iterator_init(&it, &bs);
            
            if (bson_find_fieldpath_value("transports.0", &it) == BSON_STRING) {
                strncpy(&(identity->transports[0][0]), bson_iterator_string(&it), WISH_MAX_TRANSPORT_LEN);
            }
            
            break;
        }
    } while (1); 
    wish_fs_close(fd);
    return retval;
}


// returns < 0 on error, == 0 is false, > 0 is true
int wish_identity_exists(uint8_t *uid) {
    int retval = 0;

    if (uid == NULL) {
        return -1;
    }

    wish_file_t fd = wish_fs_open(WISH_ID_DB_NAME);
    wish_offset_t prev_offset = 0;

    do {
        /* Determine length and uid of next element */
        int peek_len = sizeof (wish_identity_t) + 100;
        uint8_t peek_buf[peek_len];
        /* Re-position the stream to the end of the previous BSON structure - 
         * so that the next bytes to be read will be of the next element
         * */
        int io_retval = wish_fs_lseek(fd, prev_offset, WISH_FS_SEEK_SET);
        if (io_retval == -1) {
            WISHDEBUG(LOG_CRITICAL, "Error seeking");
            break;

        }
        io_retval = wish_fs_read(fd, peek_buf, peek_len);
        if (io_retval == 0) {
            WISHDEBUG(LOG_DEBUG, "End of file detected (2)");
            break;
        }
        else if (io_retval < 0) {
            WISHDEBUG(LOG_CRITICAL, "read error");
            break;
        }

        bson bs;
        bson_init_with_data(&bs, peek_buf);
        
        int32_t elem_len = bson_size(&bs);
        if (elem_len < 4 || elem_len > peek_len) {
            WISHDEBUG(LOG_CRITICAL, "BSON Read error");
            break;
        }
        /* Update prev offset so that we can later re-position the
         * stream */
        prev_offset+=elem_len;

        bson_iterator it;
        
        if (bson_find_from_buffer(&it, peek_buf, "uid") != BSON_BINDATA) {
            WISHDEBUG(LOG_CRITICAL, "Could not get uid (e)");
            break;
        }
        if (memcmp(bson_iterator_bin_data(&it), uid, WISH_ID_LEN) == 0) {
            WISHDEBUG(LOG_DEBUG, "Found identity (2)!");
            retval = 1;
            break;
        }
    } while (1); 
    wish_fs_close(fd);
    return retval;
}


int wish_load_identity_bson(uint8_t *uid, uint8_t *identity_bson_doc, size_t identity_bson_doc_max_len) {
    int retval = -1;

    if (uid == NULL) {
        return retval;
    }

    wish_file_t fd = wish_fs_open(WISH_ID_DB_NAME);
    wish_offset_t prev_offset = 0;

    do {
        /* Determine length and uid of next element */
        int peek_len = sizeof (wish_identity_t) + 100;
        uint8_t peek_buf[peek_len];
        /* Re-position the stream to the end of the previous BSON structure - 
         * so that the next bytes to be read will be of the next element
         * */
        int io_retval = wish_fs_lseek(fd, prev_offset, WISH_FS_SEEK_SET);
        if (io_retval == -1) {
            WISHDEBUG(LOG_CRITICAL, "Error seeking");
            break;

        }
        io_retval = wish_fs_read(fd, peek_buf, peek_len);
        if (io_retval == 0) {
            WISHDEBUG(LOG_DEBUG, "End of file detected (2)");
            break;
        }
        else if (io_retval < 0) {
            WISHDEBUG(LOG_CRITICAL, "read error");
            break;
        }

        bson bs;
        bson_init_with_data(&bs, peek_buf);
        
        int32_t elem_len = bson_size(&bs);
        if (elem_len < 4 || elem_len > peek_len) {
            WISHDEBUG(LOG_CRITICAL, "BSON Read error");
            break;
        }
        /* Update prev offset so that we can later re-position the
         * stream */
        prev_offset+=elem_len;

        bson_iterator it;
        
        if (bson_find_from_buffer(&it, peek_buf, "uid") != BSON_BINDATA) {
            WISHDEBUG(LOG_CRITICAL, "Could not get uid (f)");
            break;
        }
        if (memcmp(bson_iterator_bin_data(&it), uid, WISH_ID_LEN) == 0) {
            WISHDEBUG(LOG_DEBUG, "Found identity (3)!");
            if (identity_bson_doc_max_len >= elem_len) {
                memcpy(identity_bson_doc, peek_buf, elem_len);
                retval = 1;
            }
            else {
                WISHDEBUG(LOG_CRITICAL, "Buffer to small to copy BSON doc into!");
                retval = -1;
            }
            break;
        }
    } while (1);

    wish_fs_close(fd);
    return retval;

}

/**
 * This function calculates the uid and stores the uid matching the
 * pubkey. 
 */
void wish_pubkey2uid(const uint8_t *pubkey, uint8_t *uid) {
    mbedtls_sha256_context sha256_ctx;
    mbedtls_sha256_init(&sha256_ctx);
    mbedtls_sha256_starts(&sha256_ctx, 0); 
    mbedtls_sha256_update(&sha256_ctx, pubkey, WISH_PUBKEY_LEN); 
    mbedtls_sha256_finish(&sha256_ctx, uid);
    mbedtls_sha256_free(&sha256_ctx);
}



static void wish_create_keypair(uint8_t *pubkey, uint8_t *privkey) {
    /* Test */
#if 0
    uint8_t seed[32] = {
        0x33, 0xda, 0x14, 0xdb, 0xf3, 0x2e, 0x01, 0xff, 0xcc, 0x2e,
        0x1e, 0x83, 0x64, 0x8b, 0x2e, 0xb1, 0x31, 0x20, 0x4c, 0x7e,
        0xda, 0x94, 0x4d, 0xe8, 0x12, 0x10, 0xa0, 0x65, 0x6c, 0x29,
        0xb1, 0x44,
    };
#else
    uint8_t seed[WISH_ED25519_SEED_LEN];
    wish_platform_fill_random(NULL, seed, WISH_ED25519_SEED_LEN);
#endif

    ed25519_create_keypair(pubkey, privkey, seed);


}


void wish_create_local_identity(wish_identity_t *id, const char *alias) {
    wish_create_keypair(&(id->pubkey[0]), &(id->privkey[0]));
    id->has_privkey = true;
    wish_pubkey2uid(&(id->pubkey[0]), &(id->uid[0]));
    strncpy(&(id->alias[0]), alias, WISH_MAX_ALIAS_LEN);

    /* Encode our preferred relay server as first transport */
    wish_relay_get_preferred_server_url(&(id->transports[0][0]), WISH_MAX_TRANSPORT_LEN);
 
}

/* Return 1 if privkey is known, else 0 */
int wish_has_privkey(uint8_t *uid) {
    uint8_t privkey[WISH_PRIVKEY_LEN];
    if (wish_load_privkey(uid, privkey)) {
        return 0;
    }
    return 1;

}

int wish_load_pubkey(uint8_t *uid, uint8_t *dst_buffer) {
    wish_identity_t id;
    return_t retval = wish_identity_load(uid, &id);

    if (retval != RET_SUCCESS) {
        WISHDEBUG(LOG_CRITICAL, "wish_load_pubkey: Identity not found");
        return -1;
    }

    
    memcpy(dst_buffer, id.pubkey, WISH_PUBKEY_LEN);

    return 0;

}


int wish_load_privkey(uint8_t *uid, uint8_t *dst_buffer) {
    wish_identity_t id;
    return_t retval = wish_identity_load(uid, &id);

    if (retval != RET_SUCCESS) {
        WISHDEBUG(LOG_CRITICAL, "wish_load_privkey: Identity not found");
        return -1;
    }

    if (id.has_privkey == false) {
        WISHDEBUG(LOG_DEBUG, "Identity found, but no privkey");
        return -1;
    }

    memcpy(dst_buffer, id.privkey, WISH_PRIVKEY_LEN);
    return 0;
}

/**
 * Populate a struct wish_identity_t based on information in a 'cert'
 * which is obtained for example from a 'friend request'
 * @param new_id a pointer to the identity struct which will be
 * populated
 * @param a pointer to BSON document from which the data will be read
 * from
 * @return 0 for success
 */
int wish_identity_from_bson(wish_identity_t *id, const bson* bs) {
    //bson_visit("Populating identity from BSON:", bson_data(bs));
    if (id == NULL) {
        WISHDEBUG(LOG_CRITICAL, "new_id is null");
        return 1;
    }

    bson_iterator it;
    
    if ( bson_find(&it, bs, "pubkey") != BSON_BINDATA ) { return 1; }
    
    const uint8_t* pubkey = bson_iterator_bin_data(&it);

    if ( bson_find(&it, bs, "alias") != BSON_STRING ) { return 1; }
    
    const char* alias = bson_iterator_string(&it);

    wish_pubkey2uid(pubkey, id->uid);
    memcpy(id->pubkey, pubkey, WISH_PUBKEY_LEN);
    memcpy(id->alias, alias, strnlen(alias, WISH_MAX_ALIAS_LEN));
    id->has_privkey = false;
    memset(id->privkey, 0, WISH_PRIVKEY_LEN);

    /* Note: Transports are not part of the certificate, but are in the metadata instead. */

    /* FIXME update contacts */

    return 0;
}

void wish_identity_add_meta_from_bson(wish_identity_t *id, const bson* meta) {
    //bson_visit("Adding metadata to identity from BSON:", bson_data(meta));
    
    bson_iterator it;
    
    bson_iterator_init(&it, meta);
    /* FIXME copy transports */

    if ( bson_find_fieldpath_value("transports.0", &it) == BSON_STRING ) {
        WISHDEBUG(LOG_CRITICAL, "Copying from transprots.0: %s", bson_iterator_string(&it));
        strncpy(&(id->transports[0][0]), bson_iterator_string(&it), WISH_MAX_TRANSPORT_LEN);
        
    }
    else {
        WISHDEBUG(LOG_CRITICAL, "transprots.0 not found");
    }
}

/**
 * Remove an identity from the database 
 *
 * @param uid the uid of the identity to be removed
 * @return returns 1 if the identity was removed, or 0 for none
 */
int wish_identity_remove(wish_core_t* core, uint8_t uid[WISH_ID_LEN]) {
    int retval = 0;

    if (uid == NULL) {
        return retval;
    }

    const char *oldpath = WISH_ID_DB_NAME;
    const char *newpath = WISH_ID_DB_NAME ".tmp";
    wish_file_t old_fd = wish_fs_open(oldpath);
    wish_file_t new_fd = wish_fs_open(newpath);

    /* Truncate the new file */
    int32_t tmp = 0;
    int wr_len = wish_fs_write(new_fd, &tmp, 0);
    if (wr_len != 0) {
        WISHDEBUG(LOG_CRITICAL, "Error truncating tmp file");
    }

    wish_offset_t prev_offset = 0;

    do {
        /* Determine length and uid of next element */
        int peek_len = sizeof (wish_identity_t) + 100;
        uint8_t peek_buf[peek_len];
        /* Re-position the stream to the end of the previous BSON structure - 
         * so that the next bytes to be read will be of the next element
         * */
        int io_retval = wish_fs_lseek(old_fd, prev_offset, WISH_FS_SEEK_SET);
        if (io_retval == -1) {
            WISHDEBUG(LOG_CRITICAL, "Error seeking");
            break;

        }
        io_retval = wish_fs_read(old_fd, peek_buf, peek_len);
        if (io_retval == 0) {
            WISHDEBUG(LOG_DEBUG, "End of file detected (2)");
            break;
        }
        else if (io_retval < 0) {
            WISHDEBUG(LOG_CRITICAL, "read error");
            break;
        }

        bson bs;
        bson_init_with_data(&bs, peek_buf);
        
        int32_t elem_len = bson_size(&bs);
        if (elem_len < 4 || elem_len > peek_len) {
            WISHDEBUG(LOG_CRITICAL, "BSON Read error");
            break;
        }
        
        bson_iterator it;
        
        if (bson_find_from_buffer(&it, peek_buf, "uid") != BSON_BINDATA) {
            WISHDEBUG(LOG_CRITICAL, "Could not get uid (g)");
            break;
        }
        if (memcmp(bson_iterator_bin_data(&it), uid, WISH_ID_LEN) == 0) {
            WISHDEBUG(LOG_DEBUG, "Remove: Found identity (2)!");
            retval = 1;
        }
        else {
            /* Write the document to new file */
            wr_len = wish_fs_write(new_fd, peek_buf, elem_len);
            if (wr_len == elem_len) {
                WISHDEBUG(LOG_DEBUG, "Writing identity to new file");
            }
            else {
                WISHDEBUG(LOG_CRITICAL, "Unexpected write len!");
            }
        }
         /* Update prev offset so that we can later re-position the
         * stream */
        prev_offset+=elem_len;
    } while (1);
    
    wish_fs_close(new_fd);
    wish_fs_close(old_fd);

    wish_fs_rename(newpath, oldpath);
    
    /* For all connections: if identity is either in luid or ruid, close the connection. */
    wish_connection_t *wish_context_pool = wish_core_get_connection_pool(core);
    int i = 0;
    for (i = 0; i < WISH_CONTEXT_POOL_SZ; i++) {
        if (wish_context_pool[i].context_state == WISH_CONTEXT_FREE) {
            /* If the wish context is not in use, we can safely skip it */
            //WISHDEBUG(LOG_CRITICAL, "Skipping free wish context");
            continue;
        }
        if (memcmp(wish_context_pool[i].luid, uid, WISH_ID_LEN) == 0) {
            WISHDEBUG(LOG_CRITICAL, "identity.remove: closing context because uid is luid of a connection");
            wish_close_connection(core, &wish_context_pool[i]);
        }
        else if (memcmp(wish_context_pool[i].ruid, uid, WISH_ID_LEN) == 0) {
            WISHDEBUG(LOG_CRITICAL, "identity.remove: closing context because uid is ruid of a connection");
            wish_close_connection(core, &wish_context_pool[i]); 
        }
    }
    
    return retval;
}


void wish_identity_delete_db(void) {
    if (wish_fs_remove(WISH_ID_DB_NAME)) {
        WISHDEBUG(LOG_CRITICAL, "Unexpected while removing id db!");
    }
}

/** Get the the list of local identities, that is an array of id database entries which can be used for opening Wish connections, meaning that the privkey is also in the database.  
 * @param pointer to a caller-allocated list where result will be placed
 * @param length of the the caller-allocated list
 * @return number of local identities or 0 for an error
 */
int wish_get_local_identity_list(wish_uid_list_elem_t *list, int list_len) {
    int num_ids_in_db = wish_get_num_uid_entries();
    wish_uid_list_elem_t all_uids_list[num_ids_in_db];
    int num_uids = wish_load_uid_list(all_uids_list, num_ids_in_db);
    
    if(num_uids == 0) {
        return 0;
    }
    
    int i = 0;
    int j = 0;
    for (i = 0; i < num_uids; i++) {
        if (wish_has_privkey(all_uids_list[i].uid)) {
            memcpy(&(list[j++]), &(all_uids_list[i]), sizeof(wish_uid_list_elem_t));
        }
    }
    return j;
}

/**
 * Creates signature for data and if claim is present signature covers claim
 * 
 * @param core
 * @param uid Input
 * @param data Input
 * @param claim Input
 * @param signature Output
 * @return 
 */
return_t wish_identity_sign(wish_core_t* core, wish_identity_t* uid, const bin* data, const bin* claim, bin* signature) {
    if (!uid->has_privkey) {
        return RET_E_NO_PRIVKEY;
    }

    if (data == NULL || data->base == NULL || data->len == 0) {
        return RET_E_INVALID_INPUT;
    }
    
    int hash_len = 32;
    uint8_t hash[hash_len];
    uint8_t claim_hash[hash_len];

    mbedtls_sha256_context sha256;
    mbedtls_sha256_init(&sha256);
    mbedtls_sha256_starts(&sha256, 0); 
    mbedtls_sha256_update(&sha256, data->base, data->len); 
    mbedtls_sha256_finish(&sha256, hash);
    mbedtls_sha256_free(&sha256);


    if (claim != NULL && claim->base != NULL && claim->len > 0) {
        // If a claim is present xor it's sha256 hash with the data hash.
        // This way the signature covers the original data and the claim
        //   claim: BSON({ msg: 'This guy is good!', timestamp: Date.now(), trust: 'VERIFIED', (algo: 'sha256-ed25519') })

        mbedtls_sha256_init(&sha256);
        mbedtls_sha256_starts(&sha256, 0);
        mbedtls_sha256_update(&sha256, claim->base, claim->len);
        mbedtls_sha256_finish(&sha256, claim_hash);
        mbedtls_sha256_free(&sha256);

        int c = 0;
        for(c=0; c<32; c++) {
            hash[c] ^= claim_hash[c];
        }
    }
    
    ed25519_sign(signature->base, hash, hash_len, uid->privkey);
    signature->len = WISH_SIGNATURE_LEN;
    
    return RET_SUCCESS;
}

/**
 * Creates signature for data and if claim is present signature covers claim
 * 
 * @param core
 * @param uid Input
 * @param data Input
 * @param claim Input
 * @param signature Output
 * @return 
 */
return_t wish_identity_verify(wish_core_t* core, wish_identity_t* uid, const bin* data, const bin* claim, const bin* signature) {
    if (data == NULL || data->base == NULL || data->len == 0) {
        return RET_E_INVALID_INPUT;
    }
    
    int hash_len = 32;
    uint8_t hash[hash_len];
    uint8_t claim_hash[hash_len];

    mbedtls_sha256_context sha256;
    mbedtls_sha256_init(&sha256);
    mbedtls_sha256_starts(&sha256, 0); 
    mbedtls_sha256_update(&sha256, data->base, data->len); 
    mbedtls_sha256_finish(&sha256, hash);
    mbedtls_sha256_free(&sha256);


    if (claim != NULL && claim->base != NULL && claim->len > 0) {
        // If a claim is present xor it's sha256 hash with the data hash.
        // This way the signature covers the original data and the claim
        //   claim: BSON({ msg: 'This guy is good!', timestamp: Date.now(), trust: 'VERIFIED', (algo: 'sha256-ed25519') })

        mbedtls_sha256_init(&sha256);
        mbedtls_sha256_starts(&sha256, 0);
        mbedtls_sha256_update(&sha256, claim->base, claim->len);
        mbedtls_sha256_finish(&sha256, claim_hash);
        mbedtls_sha256_free(&sha256);

        int c = 0;
        for(c=0; c<32; c++) {
            hash[c] ^= claim_hash[c];
        }
    }
    
    if ( ed25519_verify(signature->base, hash, hash_len, uid->pubkey) ) {
        return RET_SUCCESS;
    } else {
        return RET_FAIL;
    }
}

/*
var document = {
  // exported identity
  alias: String,
  uid: Buffer(32),
  pubkey: Buffer(32),
};

var certificate = {
  // type: 'inline' | 'url',
  data: BSON(document),
  meta: BSON({ transports: ['123.234.123.234:40000'] }),
};
*/

return_t wish_identity_export(wish_core_t *core, wish_identity_t *id, bin *buffer) {
    int id_export_len = 1024;
    uint8_t id_export[id_export_len];
    bson bs;
    bson_init_buffer(&bs, id_export, id_export_len);
    bson_append_string(&bs, "alias", id->alias);
    bson_append_binary(&bs, "uid", id->uid, WISH_UID_LEN);
    bson_append_binary(&bs, "pubkey", id->pubkey, WISH_PUBKEY_LEN);
    bson_finish(&bs);

    if (bs.err) {
        WISHDEBUG(LOG_CRITICAL, "Failed while writing id_export document");
        return RET_FAIL;
    }
    
    const char *id_export_bson = bson_data(&bs);
    const int id_export_bson_len = bson_size(&bs);
    
    size_t meta_buf_len = 246;
    char meta_buf[meta_buf_len];
    bson meta;
    
    bson_init_buffer(&meta, meta_buf, meta_buf_len);
    
    wish_relay_client_t* relay;
    int i = 0;
    
    if (core->relay_db != NULL) {
        bson_append_start_array(&meta, "transports");

        LL_FOREACH(core->relay_db, relay) {
            char index[21];
            BSON_NUMSTR(index, i++);
            char host[29];
            snprintf(host, 29, "wish://%d.%d.%d.%d:%d", relay->ip.addr[0], relay->ip.addr[1], relay->ip.addr[2], relay->ip.addr[3], relay->port);

            bson_append_string(&meta, index, host);
        }

        bson_append_finish_array(&meta);
    }
    
    bson_finish(&meta);

    if (meta.err) {
        return RET_FAIL;
    }
    
    bson b;
    bson_init_buffer(&b, buffer->base, buffer->len);
       
    bson_append_binary(&b, "data", id_export_bson, id_export_bson_len);
    bson_append_binary(&b, "meta", (char *) bson_data(&meta), bson_size(&meta));

    bson_finish(&b);
    
    if (b.err) {
        return RET_FAIL;
    }
    
    return RET_SUCCESS;
}

/**
 * Helper function for building a signed certificates, usually used for friend request RPC client / RPC server handler
 * 
 * @param core
 * @param luid the uid to generate signed certificate for
 * @param result_array_name The name of the BSON array, usually "data" (when called by RPC server handler) or "args" (when called by RPC client)
 * @param buffer the buffer where the BSON result array is stored to
 * @param signed_cert_actual_len the actual length of the BSON result array is stored here
 * @return RET_SUCCESS for success, RET_FAIL for errors
 */
return_t wish_build_signed_cert(wish_core_t *core, uint8_t *luid, bin *buffer) {
    /* identity.export on the "luid" identity */
    wish_identity_t id;
    
    if (wish_identity_load(luid, &id) != RET_SUCCESS) {
        WISHDEBUG(LOG_CRITICAL, "Could not load the identity");
        return RET_FAIL;
    }
    
    size_t cert_buffer_len = 1024;
    char cert_buffer[cert_buffer_len];
    bin cert = { .base = cert_buffer, .len = cert_buffer_len };
    if (wish_identity_export(core, &id, &cert) != RET_SUCCESS) {
        WISHDEBUG(LOG_CRITICAL, "Could not export the identity");
        return RET_FAIL;
    }
   
    size_t signature_len = WISH_SIGNATURE_LEN;
    char signature_buffer[signature_len];
    bin signature = { .base = signature_buffer, .len = signature_len };
    
    if (wish_identity_sign(core, &id, &cert, NULL, &signature) != RET_SUCCESS) {
        WISHDEBUG(LOG_CRITICAL, "Could not sign the identity");
        return RET_FAIL;
    }
    
    size_t args_len = buffer->len;
    uint8_t *args = buffer->base;
    bson bs;
    bson_init_buffer(&bs, args, args_len);
    
    bson_iterator it;
    if (bson_find_from_buffer(&it, cert.base, "data") == BSON_EOO) {
        WISHDEBUG(LOG_CRITICAL, "Could not find the data element from export");
        return RET_FAIL;
    }
    // bson_visit("cert.base", cert.base);
 
    bson_append_element(&bs, NULL, &it); /* Also appends the element name */
    
    if (bson_find_from_buffer(&it, cert.base, "meta") == BSON_EOO) {
        WISHDEBUG(LOG_CRITICAL, "Could not find the meta element from export");
        return RET_FAIL;
    }
    
    bson_append_element(&bs, NULL, &it); /* Also appends the element name */
    
    bson_append_start_array(&bs, "signatures");
    bson_append_start_object(&bs, "0");
    bson_append_binary(&bs, "uid", id.uid, WISH_ID_LEN);
    bson_append_binary(&bs, "sign", signature.base, signature.len);
    bson_append_finish_object(&bs);
    bson_append_finish_array(&bs);
    
    bson_finish(&bs);
    
    if (bs.err) {
        WISHDEBUG(LOG_CRITICAL, "Could not properly write the signed cert");
        return RET_FAIL;
    }
    
    return RET_SUCCESS;
}