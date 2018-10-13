#include <stdlib.h>
#include <iostream>
#include <ctype.h>
#include <chrono>
#include <sstream>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <queue>

#include "ceps_all.hh"
#include "core/include/state_machine_simulation_core_reg_fun.hpp"
#include "core/include/state_machine_simulation_core_plugin_interface.hpp"

#include "mysql_connection.h"
#include <cppconn/driver.h>
#include <cppconn/exception.h>
#include <cppconn/resultset.h>
#include <cppconn/statement.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <stdlib.h>

using namespace std::chrono_literals;

static const std::string plugin_name = "mms_rollaut_db_check";
static const auto max_number_of_worker_threads = 1; //per host & user

static void print_usage(std::ostream& os,char * argv[]){
    using namespace std;
    os << "Usage: " << argv[0] << " [-h HOSTNAME] [-u USER] [-p PASSWD] [-t TIMESTAMP] [-c] -s SAPCODE -j JOB_NAME" << endl;
    os << endl;
    os << "Checks MySQL Table 'rollouts' for entries younger than TIMESTAMP optionally returning the value of result_cd." << endl;
    os <<"-h name of host running MySQL server, default is 'localhost'." << endl;
    os <<"-u user name used for login to MySQL server, default is 'root'." << endl;
    os <<"-p passphraseused for login to MySQL server, default is the empty string." << endl;
    os <<"-t Timestamp used for filtering of relevant job entries, entries with result_ts older are ignored." << endl;
    os <<"-c name of host running MySQL server, default is 'localhost'." << endl;

    os << "For any obligatory parameter without a value a reasonable default will be chosen." << endl;

}

template <typename T> static std::string mysql_timestamp(std::chrono::time_point<T> tmp){
    using namespace std;
    auto time_now = chrono::system_clock::to_time_t(tmp);
    auto t = std::localtime(&time_now);
    std::stringstream s;
    s << t->tm_year + 1900;
    s << "-"; if (t->tm_mon+1 < 10) s << "0";s << t->tm_mon+1;
    s << "-"; if (t->tm_mday < 10) s << "0";s << t->tm_mday;
    s << " ";
    if (t->tm_hour < 10)s << "0"; s << t->tm_hour <<":";
    if (t->tm_min < 10) s << "0"; s << t->tm_min <<":";
    if (t->tm_sec < 10) s << "0"; s << t->tm_sec;
    return s.str();
}

static void build_query(std::ostream & os ,
                        std::string timestamp,
                        std::string job_name,
                        std::string sapcode){
 os << "SELECT * from jobs WHERE "
    << " jobName = '" << job_name << "' and "
    << " sapCode = '" << sapcode << "' and "
    << " result_ts >= '" << timestamp << "' "
    //<< " start_ts <= '" << timestamp << "' "
    << " ORDER BY result_ts DESC LIMIT 1";
}

int main(int argc,char* argv[])
{
    using namespace std;

    std::string hostname = "localhost";
    std::string user = "root";
    std::string passwd = "";
    std::string timestamp = "";
    std::string sapcode;
    std::string database{"rollout"};
    std::string job_name{};
    int time_delta = 0;

    bool check_for_empty_resultset_only = true;

    int opt;

    for(; (opt = getopt(argc,argv,":h:u:p:t:d:j:s:o:c")) != -1 ;){
        if (opt == '?' || opt == ':')
        {
            print_usage(cerr,argv);
            return - EXIT_FAILURE;
        }
        switch(opt){
            case 'h' : hostname = optarg; break;
            case 'p' : passwd = optarg; break;
            case 'u' : user = optarg; break;
            case 't' : timestamp = optarg; break;
            case 'd' : database = optarg; break;
            case 'j' : job_name = optarg; break;
            case 's' : sapcode = optarg; break;
            case 'o' : time_delta = std::atoi(optarg); break;
            case 'c' : check_for_empty_resultset_only=false; break;
        }
    }

    if (timestamp.length() == 0){
        auto t = chrono::system_clock::now();
        t += std::chrono::seconds(time_delta);
        timestamp = mysql_timestamp(t);
    }

    if (job_name.length() == 0 || sapcode.length() == 0) {
        print_usage(cerr,argv);
        return - EXIT_FAILURE;
    }

    try {
      auto driver = get_driver_instance();
      std::unique_ptr<sql::Connection> con { driver->connect("tcp://"+hostname, user, passwd) };
      con->setSchema(database);
      std::stringstream query;
      build_query(query, timestamp,job_name,sapcode);

      std::unique_ptr<sql::Statement> stmt { con->createStatement() };
      std::unique_ptr<sql::ResultSet> res { stmt->executeQuery(query.str()) };
      if (check_for_empty_resultset_only)
          return res->next() ? EXIT_SUCCESS : EXIT_FAILURE;
      else if (!res->next())
          return  EXIT_FAILURE;
      //INVARIANT: We are at the very first entry in the result set
      return res->getInt("result_cd");
    } catch (sql::SQLException &e) {
      cerr << "***Fatal: " << e.what()
      << " (MySQL error code: " << e.getErrorCode()
      << ", SQLState: " << e.getSQLState() << " )" << endl;
      return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

template<typename T, typename Q> class threadsafe_queue{
    Q data_;
    mutable std::mutex m_;
    std::condition_variable cv_;
public:
    void push(T const & elem){
        std::lock_guard<std::mutex> lk(m_);
        data_.push(elem);
        cv_.notify_one();
    }

    void wait_and_pop(T & elem){
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [this]{return !data_.empty(); });
        elem = data_.front();
        data_.pop();
    }

    void wait_for_data(){
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait_for(lk,std::chrono::milliseconds(1000), [this]{return !data_.empty(); });
    }

    Q& data() {return data_;}
    std::mutex& data_mutex() const {return m_;}
};

class job_t {
public:
    std::string id;
    std::string command;
    std::string action;
    std::string hostname;
    std::string port;
    std::string authorization;
    std::string job_name;
    std::string ev_done;
    std::string ev_fail;
    std::string ev_connect_error;
    std::string ev_timeout;
    std::chrono::milliseconds timeout{0};
    std::vector< std::pair<std::string,sm4ceps_plugin_int::Variant> > params;
};

class job_ext_t :public job_t{

public:
    job_ext_t() = default;
    job_ext_t(job_t const & rhs)
    {
        *reinterpret_cast<job_t*>(this) = rhs;
    }
    std::string json_rep_of_params;
    std::string json_rep_of_params_url_encoded;
    std::chrono::steady_clock::time_point fetched;
};

struct worker_info_t{
    using queue_t = threadsafe_queue<job_t,std::queue<job_t>>;
    std::thread * worker_thread = nullptr;
    queue_t * job_queue = nullptr;
    worker_info_t() = default;
    worker_info_t(std::thread * wt,queue_t * q):worker_thread{wt},job_queue{q}{

    }
};

using worker_info_key_t = std::tuple<std::string,std::string,int>;

static std::map< worker_info_key_t , worker_info_t > workers;

Ism4ceps_plugin_interface* plugin_master;
std::mutex mysql_driver_mtx;

static bool read_http_reply(int sck,std::stringstream& data){
 constexpr auto buf_size = 32768;
 char buf[buf_size];
 auto& buffer = data;
 std::string eom = "\r\n\r\n";
 std::size_t eom_pos = 0;

 bool req_complete = false;
 ssize_t readbytes = 0;
 ssize_t buf_pos = 0;

 for(; (readbytes=recv(sck,buf,buf_size-1,0)) > 0;){
  buf[readbytes] = 0;
  std::cerr << buf;
  for(buf_pos = 0; buf_pos < readbytes; ++buf_pos){
   if (buf[buf_pos] == eom[eom_pos])++eom_pos;else eom_pos = 0;
   if (eom_pos == eom.length()){
    req_complete = true;
    if (buf_pos+1 < readbytes) buffer << buf+buf_pos+1;
    break;
   }
  }
  buffer << buf;
  if(req_complete) break;
 }

 return true;
}


static char base64set[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string encode_base64(void const * mem, size_t len){
 unsigned char * memory = (unsigned char*)mem;
 if (len == 0) return {};
 int rest = len % 3;
 size_t main_part = len - rest;
 int out_len = (len / 3) * 4;
 short unsigned int padding = 0;
 if (rest == 1) {out_len += 4;padding=2;} else if (rest == 2){ out_len +=4;padding=1;}
 std::string r;
 r.resize(out_len);
 size_t j = 0;
 size_t jo = 0;

 for(; j < main_part; j+=3,jo+=4){
  r[jo] = base64set[ *(memory + j) >> 2];
  r[jo+1] = base64set[  ( (*(memory + j) & 3) << 4)  | ( *(memory + j + 1) >> 4) ];
  r[jo+2] = base64set[ ( (*(memory + j + 1) & 0xF) << 2 )  | (*(memory + j + 2) >> 6) ];
  r[jo+3] = base64set[*(memory + j + 2) & 0x3F];
 }
 if (rest == 1){
  r[jo] = base64set[ *(memory + j) >> 2];
  r[jo+1] = base64set[ (*(memory + j) & 3) << 4];
  j+=2;jo+=2;
 } else if (rest == 2) {
  r[jo] = base64set[ *(memory + j) >> 2];
  r[jo+1] = base64set[  ( (*(memory + j) & 3) << 4)  | ( *(memory + j + 1) >> 4) ];
  r[jo+2] = base64set[ (*(memory + j + 1) & 0xF) << 2 ];
  j+=3;jo+=3;
 }
 if (padding == 1) r[jo]='='; else if (padding == 2) {r[jo] = '='; r[jo+1] = '=';}
 return r;
}

static std::string encode_base64(std::string s){
 return encode_base64((void*)s.c_str(),s.length());
}

static std::string hex_ch(int digit){
 if ( digit == 0) return "0";
 if ( digit == 1) return "1";
 if ( digit == 2) return "2";
 if ( digit == 3) return "3";
 if ( digit == 4) return "4";
 if ( digit == 5) return "5";
 if ( digit == 6) return "6";
 if ( digit == 7) return "7";
 if ( digit == 8) return "8";
 if ( digit == 9) return "9";
 if ( digit == 10) return "A";
 if ( digit == 11) return "B";
 if ( digit == 12) return "C";
 if ( digit == 13) return "D";
 if ( digit == 14) return "E";
 return "F";
}

static std::string url_encode_ascii(std::string in){
 std::stringstream ss;
 for(std::size_t i = 0; i != in.length(); ++i ){
     if (std::isalnum(in[i])){
         char buffer[2] = {0}; buffer[0] = in[i];
         ss << buffer;
     }else{
         ss << "%" << (in[i] < 16 ? "0": hex_ch(in[i] / 16) )<< hex_ch(in[i]% 16);
     }

 }
 return ss.str();
}


static std::string escape_json_string(std::string const & s){
    bool transform_necessary = false;
    for(std::size_t i = 0; i!=s.length();++i){
        auto ch = s[i];
        if (ch == '\n' || ch == '\t'|| ch == '\r' || ch == '"' || ch == '\\'){
            transform_necessary = true; break;
        }
    }
    if (!transform_necessary) return s;

    std::stringstream ss;
    for(std::size_t i = 0; i!=s.length();++i){
        char buffer[2] = {0};
        char ch = buffer[0] = s[i];
        if (ch == '\n') ss << "\\n";
        else if (ch == '\t') ss << "\\t";
        else if (ch == '\r' ) ss << "\\r";
        else if (ch == '"') ss << "\\\"";
        else if (ch == '\\') ss << "\\\\";
        else ss << buffer;
    }
    return ss.str();
}


std::string example = R"({"parameter": [{"name":"sapcode", "value":"123"}, {"name":"ROLLOUTNAME", "value":"high"}]})";



std::string test_msg_1 = "POST /job/pos_rollout_automated_002_auto_prepare_and_start_rollout_protocol/build HTTP/1.1\r\n"
        "Host: localhost:8080\r\n"
        "Authorization: Basic dG9tYXM6bEFLdGF0Mzcs\r\n"
        "User-Agent: curl/7.58.0\r\n"
        "Accept: text\r\n"
        "Jenkins-Crumb:cb0f4f9bb7847d115e07bb15317f524b\r\n"
        "Content-Length: 171\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\n\r\n"
        "json=%7B%22parameter%22%3A%20%5B%7B%22name%22%3A%22sapcode%22%2C%20%22value%22%3A%22123%22%7D%2C%20%7B%22name%22%3A%22ROLLOUTNAME%22%2C%20%22value%22%3A%22high%22%7D%5D%7D";

void control_job_thread_fn(int max_tries,
                         std::chrono::milliseconds delta,
                         std::string host, std::string user,std::string credentials, worker_info_t::queue_t* q){

    std::queue<job_ext_t> jq;
    std::queue<job_ext_t> actions;

    auto fetch_new_jobs = [&](){
            std::lock_guard<std::mutex> lk(q->data_mutex());
            for(;!q->data().empty();){
                job_ext_t e = q->data().front();
                e.fetched = std::chrono::steady_clock::now();
                if(e.params.size())
                {
                    std::stringstream ss;
                    ss << "{\"parameter\":[";
                    bool first_entry = true;
                    for(auto p: e.params){
                        if(!first_entry) ss << ",";
                        ss<<"{";
                        ss<<"\"name\":\""<<escape_json_string(p.first)<<"\",";
                        ss<<"\"value\":";
                        if(p.second.what_ == sm4ceps_plugin_int::Variant::Int)
                            ss<<p.second.iv_;
                        else if(p.second.what_ == sm4ceps_plugin_int::Variant::Double)
                            ss<<p.second.dv_;
                        else if(p.second.what_ == sm4ceps_plugin_int::Variant::String)
                            ss<<"\""<<escape_json_string(p.second.sv_)<<"\"";


                        ss<<"}";
                        first_entry = false;
                    }
                    ss << "]}";
                    e.json_rep_of_params = "json="+ss.str();
                    e.json_rep_of_params_url_encoded = "json="+url_encode_ascii(ss.str());
                }
                q->data().pop();
                if (e.action.length()==0) jq.push(e);
                else actions.push(e);
            }
    };

    for(;;){
        fetch_new_jobs();
        if(jq.empty()) {
            q->wait_for_data();
            continue;
        }

        for(;actions.size();){
            auto a = actions.front();
            actions.pop();
            if (a.action == "kill"){
                auto jobs_to_process = jq.size();
                for(;jobs_to_process;--jobs_to_process){
                    auto current_job = jq.front();jq.pop();
                    if (current_job.job_name != a.job_name || current_job.id != a.id)
                        jq.push(current_job);
                }
            }
        }
        job_ext_t current_job;
        auto jobs_to_process = jq.size();

        std::string jenkins_crumb = "cb0f4f9bb7847d115e07bb15317f524b";

        for(;jobs_to_process;--jobs_to_process){
            current_job = jq.front();
            jq.pop();

            std::string authorization = encode_base64(current_job.authorization.data(),current_job.authorization.length());//"dG9tYXM6bEFLdGF0Mzcs";


            std::stringstream ss;

            ss << "POST /job/" << current_job.job_name << "/build HTTP/1.1\r\n";
            ss << "Host: "<<current_job.hostname;
            if (current_job.port.length()) ss<<":"<<current_job.port;
            ss<< "\r\n";
            ss<<"User-Agent: RollAut/0.0.1\r\n";
            ss<<"Accept: */*\r\n";
            ss<<"Authorization: Basic "<< authorization <<"\r\n";
            ss<<"Jenkins-Crumb:"<< jenkins_crumb <<"\r\n";

            if (current_job.json_rep_of_params_url_encoded.length()){
                ss<<"Content-Length: "<<current_job.json_rep_of_params_url_encoded.length()<<"\r\n";
                ss<<"Content-Type: application/x-www-form-urlencoded\r\n\r\n";
                ss<<current_job.json_rep_of_params_url_encoded;
            } else ss<< "\r\n";

            R"(POST /job/pos_rollout_automated_002_auto_prepare_and_start_rollout_protocol/build HTTP/1.1\r\n"
                    "Host: localhost:8080\r\n"
                    "Authorization: Basic dG9tYXM6bEFLdGF0Mzcs\r\n"
                    "User-Agent: curl/7.58.0\r\n"
                    "Accept: */*\r\n"
                    "Jenkins-Crumb:cb0f4f9bb7847d115e07bb15317f524b\r\n"
                    "Content-Length: 171\r\n"
                    "Content-Type: application/x-www-form-urlencoded\r\n\r\n"
                    "json=%7B%22parameter%22%3A%20%5B%7B%22name%22%3A%22sapcode%22%2C%20%22value%22%3A%22123%22%7D%2C%20%7B%22name%22%3A%22ROLLOUTNAME%22%2C%20%22value%22%3A%22high%22%7D%5D%7D"
            )";

            auto msg = ss.str();


            addrinfo hints = {0};
            addrinfo *result, *rp;

            //memset(&hints,0,sizeof(addrinfo));
            hints.ai_canonname =nullptr;
            hints.ai_addr = nullptr;
            hints.ai_next = nullptr;
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_flags = AI_NUMERICSERV;

            if (getaddrinfo(current_job.hostname.c_str(),
                            current_job.port.c_str(),
                            &hints,
                            &result) != 0)
            {
                std::cerr<<"getaddrinfo() failed\n"<<std::endl; continue;
            }

            int cfd = -1;
            for(rp = result; rp != nullptr; rp = rp->ai_next){
                cfd = socket(rp->ai_family,rp->ai_socktype,rp->ai_protocol);
                if(cfd==-1)
                    continue;
                if(connect(cfd,rp->ai_addr,rp->ai_addrlen) != -1)
                    break;
                close(cfd);
            }

            if(rp == nullptr)
            {
                freeaddrinfo(result);
                std::cerr<<"connect() failed\n"; continue;
            }

            freeaddrinfo(result);



            if(write(cfd,msg.c_str(),msg.length()) != msg.length()){
                std::cerr<<"write() failed (msg)\n"; continue;
            }

            /*if(write(cfd,"\r\n\r\n",4) != 4){
                std::cerr<<"write() failed (trailing seq)\n"; continue;
            }*/

            std::stringstream reply;
            read_http_reply(cfd,reply);

                if (false) {
                    if (current_job.timeout.count()!=0){
                        if (std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::steady_clock::now()-current_job.fetched).count() < current_job.timeout.count())
                            jq.push(current_job);
                        else if (current_job.ev_timeout.length())
                            plugin_master->queue_event(current_job.ev_timeout,{sm4ceps_plugin_int::Variant{"Reason:Timeout"},
                                                                               sm4ceps_plugin_int::Variant{current_job.job_name},
                                                                               sm4ceps_plugin_int::Variant{current_job.id}
                                                       });
                        else
                            plugin_master->queue_event(current_job.ev_fail,{sm4ceps_plugin_int::Variant{"Reason:Timeout"},
                                                                            sm4ceps_plugin_int::Variant{current_job.job_name},
                                                                            sm4ceps_plugin_int::Variant{current_job.id}});
                    } else jq.push(current_job);
                } else {
                    auto r = 0;
                    plugin_master->queue_event(current_job.ev_done,{sm4ceps_plugin_int::Variant{(int)r}});
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(150 + rand() % 100));
        }
    }

    for(;!jq.empty();){
        auto jb = jq.front();
        jq.pop();
            plugin_master->queue_event(jb.ev_fail,{sm4ceps_plugin_int::Variant{"Reason:Failed to connect to host = '"+host+"',user = '"+"'"},
                                                   sm4ceps_plugin_int::Variant{jb.job_name},
                                                   sm4ceps_plugin_int::Variant{jb.id}
                                       });
    }

}

static void issue_job(job_t job){
    //std::cerr << "issue_job:"<< job.timestamp<<std::endl;
    static auto issue_counter = 0;

    if (job.action.length()){
        for(auto const & w : workers){
            w.second.job_queue->push(job);
        }
        return;
    }

    auto wk = worker_info_key_t{job.hostname, "",issue_counter++ % max_number_of_worker_threads};
    auto it = workers.find(wk);
    if (it == workers.end()){
        auto q = new worker_info_t::queue_t;
        workers[wk] = worker_info_t{new std::thread{control_job_thread_fn,10,std::chrono::milliseconds{10},job.hostname,"","",q}, q};
        it = workers.find(wk);
    }
    it->second.job_queue->push(job);
}

static void flatten_args(ceps::ast::Nodebase_ptr r, std::vector<ceps::ast::Nodebase_ptr>& v, char op_val = ',')
{
        if (r == nullptr) return;
        if (r->kind() == ceps::ast::Ast_node_kind::binary_operator && op(as_binop_ref(r)) ==  op_val)
        {
                auto& t = as_binop_ref(r);
                flatten_args(t.left(),v,op_val);
                flatten_args(t.right(),v,op_val);
                return;
        }
        v.push_back(r);
}








static ceps::ast::Nodebase_ptr jenkins_plugin(ceps::ast::Call_parameters* params){
    using namespace ceps::ast;
    std::vector<ceps::ast::Nodebase_ptr> args;
    if (params != nullptr && params->children().size()) flatten_args(params->children()[0], args, ',');
    //for(auto e: args) std::cout << *e << " ";
    //std::cout << std::endl;

    job_t job;
    job.ev_fail = "event_mms_rollaut_db_check_failed";
    job.ev_done = "event_mms_rollaut_db_check_done";
    job.command = "build";

    for(auto p: args){
        if (p->kind() == Ast_node_kind::binary_operator && op(as_binop_ref(p)) == '='){
            auto & root = as_binop_ref(p);
            auto  l_ = root.left(); auto r_ = root.right();
            if (l_->kind() == Ast_node_kind::symbol){
                auto & l = as_symbol_ref(l_);
                if (kind(l) != "Formal_parameter_name" && kind(l) != "Parameter") continue;
                //std::cout << name(l) << std::endl;
                auto const & lhs_name = name(l);

                if (r_->kind() == Ast_node_kind::symbol)
                {
                    auto & rhs = as_symbol_ref(r_);
                    auto const & rhs_name = name(rhs);
                    auto const & rhs_kind = kind(rhs);
                    if (rhs_kind != "Event") continue;
                    if (lhs_name == "on_error") job.ev_fail = rhs_name;
                    else if (lhs_name == "on_success") job.ev_done = rhs_name;
                    else if (lhs_name == "on_connect_error") job.ev_connect_error = rhs_name;
                    else if (lhs_name == "on_timeout") job.ev_timeout = rhs_name;
                } else {
                    if (lhs_name == "hostname" && r_->kind() == Ast_node_kind::string_literal)
                        job.hostname = value(as_string_ref(r_));
                    else if (lhs_name == "port" && r_->kind() == Ast_node_kind::string_literal)
                        job.port = value(as_string_ref(r_));
                    else if (lhs_name == "authorization" && r_->kind() == Ast_node_kind::string_literal){
                        job.authorization = value(as_string_ref(r_));
                    }
                    else if (lhs_name == "job_name" && r_->kind() == Ast_node_kind::string_literal)
                        job.job_name = value(as_string_ref(r_));
                    else if (lhs_name == "timeout_ms" && r_->kind() == Ast_node_kind::int_literal)
                        job.timeout = std::chrono::milliseconds{value(as_int_ref(r_))};
                    else if (lhs_name == "action" && r_->kind() == Ast_node_kind::string_literal)
                        job.action = value(as_string_ref(r_));
                    else if (lhs_name == "parameters"){
                        if (r_->kind()==Ast_node_kind::nodeset){
                            auto & v = as_ast_nodeset_ref(r_);
                            for(auto p : v.children()){
                                if (p->kind() != Ast_node_kind::structdef) continue;
                                auto & pp = as_struct_ref(p);
                                if(name(pp) != "param") continue;
                                Nodeset param{pp.children()};
                                if(param["name"].size() != 1 || param["name"].nodes()[0]->kind() != Ast_node_kind::string_literal) continue;
                                std::string param_name = param["name"].as_str();
                                if(param["value"].size() != 1 || ( param["value"].nodes()[0]->kind() != Ast_node_kind::string_literal
                                                                   && param["value"].nodes()[0]->kind() != Ast_node_kind::int_literal
                                                                   && param["value"].nodes()[0]->kind() != Ast_node_kind::float_literal )) continue;

                                sm4ceps_plugin_int::Variant param_value;
                                if (param["value"].nodes()[0]->kind() == Ast_node_kind::string_literal) param_value = value(as_string_ref(param["value"].nodes()[0]));
                                else if (param["value"].nodes()[0]->kind() == Ast_node_kind::int_literal) param_value = value(as_int_ref(param["value"].nodes()[0]));
                                else if (param["value"].nodes()[0]->kind() == Ast_node_kind::float_literal) param_value = value(as_double_ref(param["value"].nodes()[0]));
                                job.params.push_back({param_name,param_value});
                            }
                        }
                    }
                }
            }
        }
    }
    issue_job(job);
    return nullptr;
}

extern "C" void init_plugin(IUserdefined_function_registry* smc)
{
  (plugin_master = smc->get_plugin_interface())->reg_ceps_plugin("jenkins",jenkins_plugin);
}
