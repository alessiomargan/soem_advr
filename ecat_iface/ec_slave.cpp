#include <string.h>

#include "ec_slave.h"

//
esc_map_t esc;


void slave_factory(ec_slavet slaves[], int slavecount) {

    for ( int i = 1; i <= slavecount; i++ ) {
        if ( slaves[i].eep_man != IIT_VENDOR_ID ) {
            DPRINTF("Unrecognized product ID %d at 1-indexed position: %d\n", slaves[i].eep_man, i);
            continue;
        }

        switch ( slaves[i].eep_id ) {
            
            case IIT_Advr_test_v0_3 :
            case IIT_rt_labs :
            {
                    esc[i] = EscPtr(new Esc_test(slaves[i]));
                    break;
                }
            case IIT_Advr_HyQ_IO : {
                    esc[i] = EscPtr(new Esc_HyQ_IO(slaves[i]));
                    break;
                }
            case IIT_Advr_HyQ_Valve : {
                    esc[i] = EscPtr(new Esc_HyQ_Valve(slaves[i]));
                    break;
                }
            case IIT_Advr_BigMan : {
                    esc[i] = EscPtr(new Esc_BigMan(slaves[i]));
                    break;
                }
            default: {
                    DPRINTF("%s : Unrecognized product code %d at slave 1-indexed position: %d\n", __FUNCTION__, slaves[i].eep_id, i);
                    continue; // for loop ... so we skip _escs_type insert
                }
        }
        //int idx = esc_type[slaves[i].eep_id].size();
        //esc_type[slaves[i].eep_id][i] = idx;

    }

    //std::for_each( _escs_type.begin(), _escs_type.end(), print_map );
}

/**
 * 
 * @param _slave_arg 
 */
Esc::Esc(ec_slavet _slave_arg ) {

    DPRINTF(">> factory %x id %d conf_addr %x rev %d alias %d\n",_slave_arg.eep_man, _slave_arg.eep_id, _slave_arg.configadr & 0x0f, _slave_arg.eep_rev, _slave_arg.aliasadr);
    DPRINTF("   Ibytes %d Obytes %d\n", _slave_arg.Ibytes, _slave_arg.Obytes);

    position =      _slave_arg.configadr & 0x0f;
    vendor_id =     _slave_arg.eep_man;
    product_code =  _slave_arg.eep_id;

    inputs = _slave_arg.inputs;
    nbytes_input = _slave_arg.Ibytes;
    outputs = _slave_arg.outputs;
    nbytes_output = _slave_arg.Obytes;

    // 5 mins at 1kHz
    ec_log.set_capacity(1000 * 60 * 60 * 5);

}

Esc::~Esc() {

}

void Esc::print_IOmap() {

    DPRINTF(" O:");                  
    for ( int j = 0 ; j < nbytes_output; j++ ) {
        DPRINTF(" %2.2x", *(outputs + j));
    }

    DPRINTF(" I:");                  
    for ( int j = 0 ; j < nbytes_input; j++ ) {
        DPRINTF(" %2.2x", *(inputs + j));
    }   
    DPRINTF("\n");
}



/**
 * 
 * @param slave 
 */
Esc_test::Esc_test(ec_slavet slave ):
Esc(slave) {}


/**
 * 
 * @param slave 
 */
Esc_HyQ_IO::Esc_HyQ_IO(ec_slavet slave ):
Esc(slave) {}


/**
 * 
 * @param slave 
 */
Esc_HyQ_Valve::Esc_HyQ_Valve(ec_slavet slave ):
Esc(slave) {}


/**
 * 
 * @param slave 
 */
Esc_BigMan::Esc_BigMan(ec_slavet slave ):
    Esc(slave) {}

Esc_BigMan::~Esc_BigMan() {

    std::ofstream log_file("/tmp/log_ecat.txt");
    bigman_pdo::rx_pdo_t * ptr;
    for ( auto it=ec_log.begin(); it!=ec_log.end(); it++ ) {
        ptr = &it->bigman;
        log_file << boost::format("%1%\t%2%\t%3%\t%4%\n") % (float)ptr->_pos_ref % (float)ptr->_pos % (float)ptr->_tor_ref % (float)ptr->_tor;
    }
    log_file << std::flush;
    log_file.close();
}

