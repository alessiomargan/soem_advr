/* 
 
- init
- discovery of slaves
- pre-op
- map slaves
- start DC
- activate process data transfer
- wait for DC lock
- activate sync0
- safe-op
- wait for all slaves to reach safe-op
- op
- wait for all slaves to reach op
- do your important stuff.....

*/

#include <pthread.h>
#include <string.h>

#include "ec_master_iface.h"
#include "ec_slave.h"

namespace ec_master_iface {

/**
 * 
 */
    static uint8_t IOmap[4096];

    static int expectedWKC, g_wkc, ecat_thread_run; 

    static pthread_t        ecat_thread_id;
    static pthread_mutex_t  ecat_mutex = PTHREAD_MUTEX_INITIALIZER;

    static pthread_mutex_t  ecat_mux_sync = PTHREAD_MUTEX_INITIALIZER;
    static pthread_cond_t   ecat_cond;

    static ec_timing_t ec_timing;


    static int ecat_cycle(void) {

        int wkc;
        ec_send_processdata();
        wkc = ec_receive_processdata(EC_TIMEOUT_US);

        return wkc;
    }



/* PI calculation to get linux rx thread synced to DC time */
    static void ec_sync(const int64_t reftime, const uint64_t cycletime , int64_t* offsettime)
    {
        /* master sync offset with ec_DCtime */
        static const uint32_t sync_point_ns = 300000; //500000;
        static int64_t integral = 0;
        int64_t delta;

        delta = (reftime - sync_point_ns) % cycletime;
        if ( delta > (cycletime / 2) ) {
            delta = delta - cycletime;
        }
        if ( delta > 0 ) {
            integral++;
        }
        if ( delta < 0 ) {
            integral--;
        }
        *offsettime = -(delta / 100) - (integral / 20);

    }   

/* RT EtherCAT thread */
    void * ecat_thread( void* cycle_ns )
    {
        struct timespec   ts, tleft;
        struct timeval    tp;
        int rc;
        int ht;
        int wkc;
        uint64_t    cycle_time_ns;
        uint64_t    t_prec, t_now;
        int64_t     toff;
        uint64_t    t_delta;

#ifdef __XENO__
        pthread_set_name_np(pthread_self(), "ecat");
        pthread_set_mode_np(0, PTHREAD_WARNSW);
#else
        pthread_setname_np(pthread_self(), "ecat");
#endif


        //rc = pthread_mutex_lock(&mutex);
        rc = clock_gettime(CLOCK_MONOTONIC, &ts);
        ht = (ts.tv_nsec / 1000000) + 1; /* round to nearest ms */
        ts.tv_nsec = ht * 1000000;
        cycle_time_ns = *((uint64_t*)(cycle_ns)); /* cycletime in ns */
        //DPRINTF("%llu \n", cycle_time_ns);
        toff = 0;
        ec_send_processdata();    

        while ( ecat_thread_run ) {
            /* calculate next cycle start */
            add_timespec(&ts, cycle_time_ns + toff);
            /* wait to cycle start */
            rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);

            pthread_mutex_lock(&ecat_mutex);
            //wkc = ec_receive_processdata(EC_TIMEOUTRET);
            wkc = ecat_cycle();
            pthread_mutex_unlock(&ecat_mutex);

             if ( ec_slave[0].hasdc && cycle_time_ns != 0 ) {
                /* calulate toff to get linux time and DC synced */
                ec_sync(ec_DCtime, cycle_time_ns, &toff);
                t_now = get_time_ns();
                t_delta =  t_now - t_prec;
                t_prec = t_now;

                ec_timing.recv_dc_time = ec_DCtime;
                ec_timing.offset = toff;
                ec_timing.loop_time = t_delta;
            } else {
                toff = 250000;
            }

            if ( wkc >= expectedWKC ) {

                pthread_mutex_lock(&ecat_mux_sync);
                pthread_cond_signal(&ecat_cond);
                pthread_mutex_unlock(&ecat_mux_sync);

            } else {
                //DPRINTF("wkc %d\n", wkc);
            }

        }    
    }


    static void start_ecat_thread(const uint64_t* cycle_time_ns) {

        pthread_attr_t      attr;
        int                 policy;
        cpu_set_t           cpu_set;
        struct sched_param  schedparam;


#ifdef __XENO__
        policy = SCHED_FIFO;
#else
        policy = SCHED_OTHER;
#endif

        CPU_ZERO(&cpu_set);
        CPU_SET(1,&cpu_set);

        pthread_attr_init(&attr);
        pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setschedpolicy(&attr, policy);
        schedparam.sched_priority = sched_get_priority_max(policy);
        pthread_attr_setschedparam(&attr, &schedparam);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
        pthread_attr_setaffinity_np(&attr, sizeof(cpu_set), &cpu_set);

        DPRINTF("[ECat_master] Start ecat_thread %llu ns\n", *cycle_time_ns);
        ecat_thread_run = 1;
        pthread_create(&ecat_thread_id, &attr, ecat_thread, (void*)cycle_time_ns);

    }

    bool req_state_check(uint16 slave, uint16_t req_state) {

        uint16_t act_state;
        uint16_t ec_error_mask = 0x10;
        uint16_t ec_state_mask = 0x0F;

        if (slave == 0) {
            DPRINTF("[ECat_master] Request 0x%02X state for all slaves\n", req_state);
        } else {
            DPRINTF("[ECat_master] Request 0x%02X state for %d slave\n", req_state, slave);
        }

        ec_slave[slave].state = req_state;
        ec_writestate(slave);
        // just check req_state ... no error indication bit is check
        act_state = ec_statecheck(slave, req_state,  EC_TIMEOUTSTATE * 3); 

        if ( req_state != act_state) {
            // not all slave reached requested state ... find who and check error indication bit
            ec_readstate();

            if (slave == 0) {

                for ( int i = 1; i<=ec_slavecount ; i++ ) {
                    if ( ec_slave[i].state != req_state ) {
                        DPRINTF("Slave %d State=0x%02X StatusCode=0x%04X : %s\n",
                               i, ec_slave[i].state, ec_slave[i].ALstatuscode, ec_ALstatuscode2string(ec_slave[i].ALstatuscode));

                        if (ec_slave[i].state & ec_error_mask) {
                            // attemping to ack
                            ec_slave[i].state = (req_state & ec_state_mask) + EC_STATE_ACK;
                            ec_writestate(i);
                            act_state = ec_statecheck(i, req_state,  EC_TIMEOUTSTATE * 3); 
                            if ( req_state != act_state) {
                                // still req_state not reached ...
                                DPRINTF("... Slave %d State=0x%02X StatusCode=0x%04X : %s\n",
                                        i, ec_slave[i].state, ec_slave[i].ALstatuscode, ec_ALstatuscode2string(ec_slave[i].ALstatuscode));
                            }
                        }


                    }
                }
            } else {
                DPRINTF("Slave %d State=0x%02X StatusCode=0x%04X : %s\n",
                       slave, ec_slave[slave].state, ec_slave[slave].ALstatuscode, ec_ALstatuscode2string(ec_slave[slave].ALstatuscode));

                if (ec_slave[slave].state & ec_error_mask) {
                    // attemping to ack
                    ec_slave[slave].state = (req_state & ec_state_mask) + EC_STATE_ACK;
                    ec_writestate(slave);
                    act_state = ec_statecheck(slave, req_state,  EC_TIMEOUTSTATE * 3); 
                    if ( req_state != act_state) {
                        // still req_state not reached ...
                        DPRINTF("... Slave %d State=0x%02X StatusCode=0x%04X : %s\n",
                                slave, ec_slave[slave].state, ec_slave[slave].ALstatuscode, ec_ALstatuscode2string(ec_slave[slave].ALstatuscode));
                    }
                }

            }

        }

        return (act_state == req_state);
    }
/**
 * 
 * 
 * @author amargan (3/31/2014)
 * 
 * @param ifname 
 * @param ecat_cycle_ns 
 * @param ecat_cycle_shift_ns 
 * 
 * @return int expectedWKC
 */
    int initialize(const char* ifname, const uint64_t* ecat_cycle_ns, const uint64_t* ecat_cycle_shift_ns) {


        DPRINTF("[ECat_master] Using %s\n", ifname);
        if ( ! ec_init((char*)ifname) ) {
            DPRINTF("[ECat_master] ECat_rt_soem_Master: ec_init(%s) failed!\n", ifname);
            return 0;
        }

        // retunr workcounter of slave discover datagram = number of slaves found
        ec_config(FALSE, &IOmap);

        DPRINTF("[ECat_master] %d EtherCAT slaves identified.\n", ec_slavecount);
        if ( ec_slavecount < 1 ) {
            DPRINTF("[ECat_master] Failed to identify any slaves! Failing to init.\n");
            return 0;
        }

        // map slaves 
        slave_factory(ec_slave, ec_slavecount);

        req_state_check(0, EC_STATE_PRE_OP);

        if ( ! ec_configdc() ) {
            DPRINTF("[ECat_master] Failed to config DC\n");
        }

        // Configure DC if ...
        if ( *ecat_cycle_ns > 0 ) {
            DPRINTF("[ECat_master] Configure DC\n");
            // Configure the distributed clocks for each slave.
            for ( int i = 1; i <= ec_slavecount; i++ ) {
                ec_dcsync0(i, true, *ecat_cycle_ns, *ecat_cycle_shift_ns);
            }
        }

        req_state_check(0, EC_STATE_SAFE_OP);

        if ( ! req_state_check(0, EC_STATE_OPERATIONAL) ) {
            // exit .. otherwise with stuck in next loop
            // !! if the bootloader is running the only allowed state are INIT and BOOT
            return 0;
        }

        // We are now in OP ...

        if ( *ecat_cycle_ns > 0 ) {
            // Update ec_DCtime so we can calculate stop time below.
            ecat_cycle();
            // Send a barrage of packets to set up the DC clock.
            int64_t stoptime = ec_DCtime + *ecat_cycle_shift_ns/2;
            // SOEM automatically updates ec_DCtime.
            while ( ec_DCtime < stoptime ) {
                ecat_cycle();
            }
        }
        // We now have data.

        expectedWKC = (ec_group[0].outputsWKC * 2) + ec_group[0].inputsWKC;
        DPRINTF("[ECat_master] Calculated workcounter %d\n", expectedWKC);
        DPRINTF("[ECat_master] ec_DCtime %d\n", ec_DCtime);
        DPRINTF("[ECat_master] o: %d   i: %d\n", ec_slave[0].Obytes, ec_slave[0].Ibytes);


        pthread_mutex_init(&ecat_mutex, NULL);
        pthread_mutex_init(&ecat_mux_sync, NULL);
        pthread_cond_init(&ecat_cond, NULL);
        // start thread, setting distribuited clock DC0 to 1 ms
        start_ecat_thread(ecat_cycle_ns);


        return expectedWKC;

    }



    void finalize(void) {

        DPRINTF("[ECat_master] Stop ecat_thread\n");
        ecat_thread_run = 0;
        pthread_join(ecat_thread_id, NULL);

        for ( int i = 1; i <= ec_slavecount; i++ ) {
            ec_dcsync0(i, FALSE, 0, 0); // SYNC0 off
        }

        req_state_check(0, EC_STATE_INIT);

        ec_close();
        DPRINTF("[ECat_master] close\n");

    }


    int recv_from_slaves(output_slave_t* slave_outputs, ec_timing_t* timing) {

        int ret;
        // Xenomai pthread_cond_timedwait()
        // The timeout abstime is expressed as an absolute value of the clock attribute passed to pthread_cond_init().
        // By default, CLOCK_REALTIME is used.
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec = ts.tv_sec + 1;

        pthread_mutex_lock(&ecat_mux_sync);
        ret = pthread_cond_timedwait(&ecat_cond, &ecat_mux_sync, &ts);
        pthread_mutex_unlock(&ecat_mux_sync);

        *timing = ec_timing;

        //ret 0 on success,
        if ( ! ret ) {
            for ( auto it = esc.begin(); it != esc.end(); it++ ) {
                switch ( it->second->product_code ) {
                    
                    case IIT_Advr_test_v0_3 :
                    case IIT_rt_labs :
                        it->second->get_slave_outputs(slave_outputs[it->second->position-1].test);
                        break;

                    case IIT_Advr_HyQ_IO : 
                        it->second->get_slave_outputs(slave_outputs[it->second->position-1].hyq_io);
                        break;

                    case IIT_Advr_HyQ_Valve:
                        it->second->get_slave_outputs(slave_outputs[it->second->position-1].hyq_valve);
                        break;

                    case IIT_Advr_BigMan:
                        it->second->get_slave_outputs(slave_outputs[it->second->position-1].bigman);
                        break;
                }
                it->second->ec_log.push_back(slave_outputs[it->second->position-1]);
            }
        }

        // ret < 0 on error
        return ret;
    }

    int send_to_slaves(input_slave_t* slave_inputs) {

        int wkc;

        for ( auto it = esc.begin(); it != esc.end(); it++ ) {
            switch ( it->second->product_code ) {
                
                case IIT_Advr_test_v0_3 :
                case IIT_rt_labs :
                    it->second->set_slave_inputs(slave_inputs[it->second->position-1].test);
                    break;

                case IIT_Advr_HyQ_IO : 
                    it->second->set_slave_inputs(slave_inputs[it->second->position-1].hyq_io);
                    break;

                case IIT_Advr_HyQ_Valve:
                    it->second->set_slave_inputs(slave_inputs[it->second->position-1].hyq_valve);
                    break;

                case IIT_Advr_BigMan:
                    it->second->set_slave_inputs(slave_inputs[it->second->position-1].bigman);
                    break;
            }
        }

        pthread_mutex_lock(&ecat_mutex);
        //ec_send_processdata();
        wkc = ecat_cycle();
        pthread_mutex_unlock(&ecat_mutex);

        return wkc;

    }

    int update_slave_firmware(uint16_t slave, std::string firmware, uint32_t passwd_firm) {

        req_state_check(0, EC_STATE_INIT);
        
        // first boot state request is handled by application that jump to bootloader
        // we do NOT have a state change in the slave
        req_state_check(slave, EC_STATE_BOOT);

        // second boot state request is handled by bootloader
        // now the slave should go in BOOT state
        if ( ! req_state_check(slave, EC_STATE_BOOT) ) {
            DPRINTF("Slave %d not changed to BOOT state.\n", slave);
            return 0;
        }

        std::ifstream firm(firmware);
        std::string firm_buff((std::istreambuf_iterator<char>(firm)), std::istreambuf_iterator<char>());

        DPRINTF("File read OK, %d bytes.\n",firm_buff.length());
        DPRINTF("Update ....\n");
        int result = ec_FOEwrite(slave, (char*)firmware.c_str(), passwd_firm, firm_buff.length() ,(void*)firm_buff.c_str(), EC_TIMEOUTSTATE);
        if (result < 0 ) 
            DPRINTF("Fail with code %d\n", result);

        req_state_check(slave, EC_STATE_INIT);

        return result;
    }
}
