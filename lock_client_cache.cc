// RPC stubs for clients to talk to lock_server, and cache the locks
// see lock_client.cache.h for protocol details.

#include "lock_client_cache.h"
#include "rpc.h"
#include <sstream>
#include <iostream>
#include <stdio.h>
#include <unistd.h>


static void *
releasethread(void *x)
{
    lock_client_cache *cc = (lock_client_cache *) x;
    cc->releaser();
    return 0;
}

int lock_client_cache::last_port = 0;


lock_client_cache::lock_client_cache(std::string xdst,
                                     class lock_release_user *_lu)
: lock_client(xdst), lu(_lu)
{
    srand(time(NULL)^last_port);
    rlock_port = ((rand()%32000) | (0x1 << 10));
    const char *hname;
    // assert(gethostname(hname, 100) == 0);
    hname = "127.0.0.1";
    std::ostringstream host;
    host << hname << ":" << rlock_port;
    id = host.str();
    last_port = rlock_port;
    rpcs *rlsrpc = new rpcs(rlock_port);
    /* register RPC handlers with rlsrpc */
    rlsrpc->reg(rlock_protocol::revoke, this, &lock_client_cache::accept_revoke_request);
    rlsrpc->reg(rlock_protocol::retry, this, &lock_client_cache::accept_retry_request);
    
    pthread_t th;
    int r = pthread_create(&th, NULL, &releasethread, (void *) this);
    assert (r == 0);
}


struct delay_thread_struct {
    lock_client_cache *cc;
    lock_protocol::lockid_t lid;
    //we can also put a sequence number here
};

static void *
delaythread(void *x)
{
    //wait here for 0.1 seconds
    
    usleep(10000 + (rand() % 10000));
    
    delay_thread_struct *ptr = (delay_thread_struct *) x;
    ptr->cc->release_to_lock_server(0, ptr->lid);
    delete ptr;
    return 0;
}


void
lock_client_cache::releaser()
{
    
    // This method should be a continuous loop, waiting to be notified of
    // freed locks that have been revoked by the server, so that it can
    // send a release RPC.
    
    lock_protocol::lockid_t lid;
    while (1) {
        // std::cout << "lock_client::releaser 1\n";
        lid = releaser_queue.pop_front();
        // std::cout << "lock_client::releaser 2\n";
        // send release
        
        delay_thread_struct *t = new delay_thread_struct();
        t->cc = this;
        t->lid = lid;
        
        pthread_t th;
        int r = pthread_create(&th, NULL, &delaythread, (void *) t);
        assert (r == 0);
        
    }
    
}


lock_protocol::status
lock_client_cache::acquire(lock_protocol::lockid_t lid) {
    pthread_mutex_lock(&release_acquire_mutex);
    // seqnum += 1;
    
    std::map<lock_protocol::lockid_t, lock>::iterator it;
    
    it = locks.find(lid);
    
    if (it == locks.end()) {
        lock lock_;
        lock_.lock_state = lock::NONE;
        locks[lid] = lock_;
        it = locks.find(lid);
    }
    
    while(1) {
        if (it->second.lock_state == lock::ACQUIRING
            || it->second.lock_state == lock::LOCKED
            || it->second.lock_state == lock::RELEASING) {
            //lock_state might have changed to NONE and then to FREE while we were sleeping
            pthread_cond_wait(&(it->second.cond_var), &release_acquire_mutex);
            continue;
        } else if (it->second.lock_state == lock::NONE) {
            it->second.lock_state = lock::ACQUIRING;
            while (1) {
                int ret;
                int r;
                // should consider unlocking mutex here, though that
                // leads to retrier signaling before we go to sleep
                ret = cl->call(lock_protocol::acquire, cl->id(), id, seqnum, lid, r);
                if (r == lock_protocol::OK) {
                    std::cout << id << " lock_client_cache::acquire: acquired " << lid << std::endl;
                    it->second.lock_state = lock::LOCKED;
                    n_successes += 1;
                    std::cout << "N_SUCCESSES " << n_successes << std::endl;
                    break;
                }
                else {
                    n_failures += 1;
                    std::cout << "N_FAILURES " << n_failures << std::endl;
                    std::cout << id << " lock_client_cache::acquire: retry later" << lid << std::endl;
                }
                pthread_cond_wait(&(it->second.cond_var), &release_acquire_mutex);
            }
        } else if (it->second.lock_state == lock::FREE) {
            it->second.lock_state = lock::LOCKED;
            std::cout << id << " lock_client_cache::acquire: acquired from cache" << lid << std::endl;
        } else {
            throw "Something is wrong with [it->second.lock_state]!";
        }
        break; 
    }
    pthread_mutex_unlock(&release_acquire_mutex);
    
    return lock_protocol::OK;
}

lock_protocol::status
lock_client_cache::release(lock_protocol::lockid_t lid) {
    
    pthread_mutex_lock(&release_acquire_mutex);
    
    std::cout << id <<  " lock_client_cache::release: released to cache " << lid << std::endl;
    
    locks[lid].lock_state = lock::FREE;
    pthread_cond_broadcast(&locks[lid].cond_var);
    
    pthread_mutex_unlock(&release_acquire_mutex);
    
    return lock_protocol::OK;
}

rlock_protocol::status
lock_client_cache::accept_retry_request(rlock_protocol::seqnum_t seqnum, lock_protocol::lockid_t lid, int &r)
{
    std::cout << id << " can retry now: seqnum " << seqnum << " lid " << lid << std::endl;
    
    // without this mutex we signal before acquiring thread goes to sleep
    pthread_mutex_lock(&release_acquire_mutex);
    pthread_cond_broadcast(&locks[lid].cond_var);
    pthread_mutex_unlock(&release_acquire_mutex);
    return rlock_protocol::OK;
}

rlock_protocol::status
lock_client_cache::accept_revoke_request(rlock_protocol::seqnum_t seqnum, lock_protocol::lockid_t lid, int &r)
{
    releaser_queue.push_back(lid);
    
    return rlock_protocol::OK;
}

rlock_protocol::status
lock_client_cache::release_to_lock_server(rlock_protocol::seqnum_t seqnum, lock_protocol::lockid_t lid) {
    std::cout << id << "lock_client_cache::release_to_lock_server" << std::endl;
    pthread_mutex_lock(&release_acquire_mutex);
    
    std::map<lock_protocol::lockid_t, lock>::iterator it;
    
    it = locks.find(lid);
    
    while(1) {
        if (it->second.lock_state == lock::ACQUIRING || it->second.lock_state == lock::LOCKED) {
            pthread_cond_wait(&(it->second.cond_var), &release_acquire_mutex);
            continue;
        } else if (it->second.lock_state == lock::FREE) {
            std::cout << id << "lock_client_cache::release_to_lock_server::FREE" << std::endl;
            it->second.lock_state = lock::RELEASING;
            
            pthread_mutex_unlock(&release_acquire_mutex);
            
            // flush to extent
            lu->dorelease(lid);
            usleep(100000);
            
            int ret;
            int r;
            ret = cl->call(lock_protocol::release, cl->id(), id, seqnum, lid, r);
            
            assert (ret == 0);
            
            pthread_mutex_lock(&release_acquire_mutex);
            
            std::cout << id << " lock_client_cache::release_to_lock_server: released seqnum " << seqnum << " lid " << lid << std::endl;
            it->second.lock_state = lock::NONE;
            pthread_cond_broadcast(&locks[lid].cond_var);
            
        } else if (it->second.lock_state == lock::NONE
                   || it->second.lock_state == lock::RELEASING) {
            // do nothing, it was already released, might cause problems if reordering
        } else {
            throw "Something is wrong with [it->second.lock_state]!";
        }
        break;
    }
    pthread_mutex_unlock(&release_acquire_mutex);
    return lock_protocol::OK;
}
