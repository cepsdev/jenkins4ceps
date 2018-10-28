#include <stdlib.h>
#include <iostream>
#include <ctype.h>
#include <chrono>
#include <sstream>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <queue>
#include <unordered_map>
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
#include "rapidjson/rapidjson.h"
#include "rapidjson/reader.h"

using namespace std::chrono_literals;

static const std::string plugin_name = "mms_rollaut_db_check";

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

using worker_info_key_t = std::tuple<std::string,std::string,int>;

class job_t {
public:
    struct authorization_t{
        std::string crumb;
        std::string authorization;
    };

    worker_info_key_t worker_id; //Each job is associated to a worker thread. In the various processing stages a job jumps between different queues, but the the job's worker thread association
    //stays the same for ever.
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
    std::string ev_job_queued;

    std::chrono::milliseconds timeout{0};
    std::vector< std::pair<std::string,sm4ceps_plugin_int::Variant> > params;
    bool follow_status = false;
    authorization_t auth;
    bool authorization_complete(){
        return auth.crumb.length();
    }

};

class job_ext_t :public job_t{

public:
    job_ext_t() = default;
    job_ext_t(job_t const & rhs)
    {
        *reinterpret_cast<job_t*>(this) = rhs;
    }
    job_ext_t(job_ext_t const & rhs) = default;

    std::string json_rep_of_params;
    std::string json_rep_of_params_url_encoded;
    std::chrono::system_clock::time_point fetched;
    std::chrono::system_clock::time_point triggered;

    std::chrono::high_resolution_clock::time_point fetched_hr;
    std::chrono::high_resolution_clock::time_point triggered_hr;
    std::chrono::high_resolution_clock::time_point queued_hr;


    long long associated_buildnumber = -1;
    std::string key;
    bool watch = false;
    int ticks = 100;
};

struct worker_info_t{
    using queue_t = threadsafe_queue<job_t,std::queue<job_t>>;
    std::thread * worker_thread = nullptr;
    queue_t * job_queue = nullptr;
    worker_info_t() = default;
    worker_info_t(std::thread * wt,queue_t * q):worker_thread{wt},job_queue{q}{

    }
};

struct authenticator_info_t{
    using queue_t = threadsafe_queue<job_ext_t,std::queue<job_ext_t>>;
    std::thread * worker_thread = nullptr;
    queue_t * job_queue = nullptr;
    authenticator_info_t() = default;
    authenticator_info_t(std::thread * wt,queue_t * q):worker_thread{wt},job_queue{q}{
    }
};


class Job_Control{
public:
    virtual bool push_to_worker_queue(job_ext_t const & job) = 0;
    virtual bool push_to_authenticator_queue(job_ext_t const & job) = 0;
    virtual void ev(std::string ev_name,std::initializer_list<sm4ceps_plugin_int::Variant> vl = {}) = 0;
};

class Jenkinsplugin:public Job_Control {
    std::map< worker_info_key_t , worker_info_t > workers;
    std::map< std::pair<std::string,std::string>, authenticator_info_t > authenticators;
    std::map< std::pair<std::string,std::string>, std::string > host_and_port2crumbs;
    mutable std::mutex host_and_port2crumbs_mtx;

    int issue_counter = 0;
    int max_number_of_worker_threads = 1; //per host & user
    void authenticator_thread(authenticator_info_t::queue_t* job_queue);
public:
    void issue_job(job_t job);
    bool push_to_worker_queue(job_ext_t const & job) override;
    bool push_to_authenticator_queue(job_ext_t const & job) override;
    void ev(std::string ev_name,std::initializer_list<sm4ceps_plugin_int::Variant> vl = {}) override;
};

static Jenkinsplugin jenkinsplugin;




Ism4ceps_plugin_interface* plugin_master;
std::mutex mysql_driver_mtx;

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


static std::pair<bool,std::string> get_virtual_can_attribute_content(std::string attr, std::vector<std::pair<std::string,std::string>> const & http_header){
 for(auto const & v : http_header){
     if (v.first == attr)
         return {true,v.second};
 }
 return {false,{}};
}

class http_reply_t {
  public:
    int MAX_CONTENT_SIZE = 1024768;

    using attribute_t = std::pair<std::string,std::string>;
    std::string header;
    std::vector<std::pair<std::string,std::string>> attributes;
    int reply_code;
    int content_length;
    std::stringstream content;
};

static http_reply_t
       read_http_reply(int sck,bool debug = true){

debug = false;
 http_reply_t r;
 constexpr auto buf_size = 4;
 char buf[buf_size+1] = {0};
 std::stringstream stream_buffer;
 std::string eom = "\r\n\r\n";
 std::size_t eom_pos = 0;

 bool req_complete = false;
 ssize_t readbytes = 0;
 ssize_t buf_pos = 0;
 int content_read = 0;
 char overwritten_char;


 for(; (readbytes=recv(sck,buf,buf_size,0)) > 0;){
  buf[readbytes] = 0;
  if (debug){
      std::cerr << buf;
  }
  for(buf_pos = 0; buf_pos < readbytes; ++buf_pos){
   if (buf[buf_pos] == eom[eom_pos])++eom_pos;else eom_pos = 0;
   if (eom_pos == eom.length()){
    req_complete = true;
    if ( buf_pos+1 < readbytes){
        content_read = readbytes - (buf_pos+1);
    }
    overwritten_char = buf[buf_pos+1];
    buf[buf_pos+1] = 0;
    break;
   }
  }
  stream_buffer << buf;

  if(req_complete) break;
 }

 auto buffer = stream_buffer.str();
 int content_length{};
 bool chunked = false;

 if (!req_complete) return r;

 {
  std::string first_line;
  size_t line_start = 0;
  for(size_t i = 0; i < buffer.length();++i){
    if (i+1 < buffer.length() && buffer[i] == '\r' && buffer[i+1] == '\n' ){
        if (line_start == 0) first_line = buffer.substr(line_start,i);
        else if (line_start != i){
         std::string attribute;
         std::string content;
         std::size_t j = line_start;
         for(;j < i && buffer[j]==' ';++j);
         auto attr_start = j;
         for(;j < i && buffer[j]!=':';++j);
         attribute = buffer.substr(attr_start,j-attr_start);
         ++j; //INVARIANT: buffer[j] == ':' || j == i
         for(;j < i && buffer[j]==' ' ;++j);
         auto cont_start = j;
         auto cont_end = i - 1;
         for(;buffer[cont_end] == ' ';--cont_end);
         if ( cont_start <= cont_end) content = buffer.substr(cont_start, cont_end - cont_start + 1);
         r.attributes.push_back(std::make_pair(attribute,content));
         if (attribute == "Content-Length")
             content_length = std::atoi(content.c_str());
         else if (attribute == "Transfer-Encoding" && content == "chunked") chunked = true;
        }
        line_start = i + 2;++i;
    }
  }
  r.header = first_line;
 }


 r.reply_code = -1; //Unknown
 //Get Reply Code
 {
     auto start_rcode = r.header.find_first_of(" ");
     if (start_rcode != std::string::npos) for(;start_rcode < r.header.length() && r.header[start_rcode] == ' ';++start_rcode);

     if (start_rcode != std::string::npos && start_rcode < r.header.length()){
         auto end_rcode = r.header.find_first_of(" ",start_rcode);
         if (end_rcode != std::string::npos) r.reply_code = std::stoi(r.header.substr(start_rcode,end_rcode - start_rcode));
     }
 }


 if (content_read && !chunked){
     buf[buf_pos+1] = overwritten_char;
     r.content << buf+(buf_pos+1);
 }

 if (content_length-content_read > 0){

     auto bytes_left = std::min(r.MAX_CONTENT_SIZE,content_length-content_read);
     for(; bytes_left ; bytes_left-=readbytes){
      readbytes=recv(sck,buf,std::min(bytes_left,buf_size),0);
      buf[readbytes] = 0;
      if (debug){
          std::cerr << buf;
      }
      r.content << buf;
     }
 } else if (chunked){
    if(content_read){
      buf[buf_pos+1] = overwritten_char;
      //std::cerr <<">>"<< (buf+buf_pos+1) << std::endl;
      auto buf_start = buf_pos + 1;
      auto buf_len = content_read;
      for(;;){
          int bytes_left = 0;
          {
              ssize_t chunk_length = 0;
              auto i = 0;
              bool rn_found=false;
              for(;i+1 < buf_len;++i){
                  rn_found = buf[buf_start+i] == '\r' && buf[buf_start+i+1] == '\n';
                  if(rn_found) break;
              }
              if(!rn_found){
                  char buffer[128] = {0};
                  strcpy(buffer,buf+buf_start);
                  int i = strlen(buffer)-1;
                  bool r_read = buffer[i] == '\r';
                  int readbytes=0;
                  for(;;){
                      readbytes = recv(sck,buffer+i+1,1,0);
                      if (readbytes <= 0) return r;
                      ++i;
                      if(r_read && buffer[i]=='\n') break;
                      if(r_read && buffer[i] != '\n') r_read = false;
                      if(!r_read && buffer[i] == '\r') r_read = true;
                  }
                  chunk_length = strtol(buffer,nullptr,16);
                  if (chunk_length == 0) break;
                  bytes_left=chunk_length;
              }else{
                  chunk_length = strtol(buf+buf_start,nullptr,16);
                  if (chunk_length == 0) break;
                  buf_start += i+2;
                  bytes_left = chunk_length - (buf_len - i - 2 );
                  if (bytes_left < 0){
                      char t = buf[buf_start + chunk_length];
                      buf[buf_start + chunk_length] = 0;
                      r.content << buf+buf_start;
                      buf[buf_start + chunk_length] = t;
                      buf_start += chunk_length + 2;
                      buf_len -= i+2+chunk_length + 2;
                      continue;
                  }
                  if (chunk_length>0) r.content << buf+buf_start;
              }
          }//chunk_start
          auto readbytes = 0;
          for(; bytes_left>0 ; bytes_left-=readbytes){
           readbytes=recv(sck,buf,std::min(bytes_left,buf_size),0);
           if (readbytes <= 0) break;
           buf[readbytes] = 0;
           if (debug){
               std::cerr << buf;
           }
           r.content << buf;
          }
          readbytes=recv(sck,buf,2,0);// skip leading \r\n
          if (readbytes != 2) break;
          readbytes=recv(sck,buf,buf_size,0);// skip leading \r\n

          if (readbytes > 0){
            buf[readbytes] = 0;
            buf_start = 0;
            buf_len = readbytes;
          } else  break;
      }
    }

 }//chunked


 r.content_length = r.content.str().length();

 return r;
}


class http_request_and_reply{
public:
    int sck = -1;
    std::string hostname = "localhost";
    std::string port = "80";
    http_reply_t http_reply;
    bool debug = false;
    enum Resultcode {
        UNDEFINED,
        OK,
        ERR_CONNECT,
        ERR_WRITE,
        ERR_READ
        };
    Resultcode last_result = UNDEFINED;
    static std::string result_str(Resultcode r){
        if (r == OK) return "Ok";
        else if (r == ERR_CONNECT) return "Connect failed";
        else if (r == ERR_WRITE) return "write() failed";
        else if (r == ERR_WRITE) return "read() failed";
        return "Unknown";
    }
    Resultcode start(std::string const & msg);
    http_request_and_reply() = default;
    http_request_and_reply(std::string h, std::string p):hostname{h},port{p} {}
    http_request_and_reply(std::string h, std::string p,std::string const & msg, bool d = false):hostname{h},port{p},debug{d} {
        start(msg);
    }
    ~http_request_and_reply(){
        if(sck != -1) close(sck);sck = -1;
    }
};

http_request_and_reply::Resultcode http_request_and_reply::start(std::string const & msg){
    addrinfo hints = {0};
    addrinfo *result, *rp;
    hints.ai_canonname =nullptr;
    hints.ai_addr = nullptr;
    hints.ai_next = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;


    if (getaddrinfo(hostname.c_str(),
                    port.c_str(),
                    &hints,
                    &result) != 0)
     return last_result = Resultcode::ERR_CONNECT;

    for(rp = result; rp != nullptr; rp = rp->ai_next){
        sck = socket(rp->ai_family,rp->ai_socktype,rp->ai_protocol);
        if(sck==-1)
            continue;
        if(connect(sck,rp->ai_addr,rp->ai_addrlen) != -1)
            break;
        close(sck);
    }

    if(rp == nullptr)
    {
        freeaddrinfo(result);
        return last_result = Resultcode::ERR_CONNECT;
    }
    freeaddrinfo(result);
    if (debug){
        std::cerr << ">>"<<hostname<<":"<<port<<std::endl;
        std::cerr << msg << std::endl<<std::endl;
    }
    if(write(sck,msg.c_str(),msg.length()) != msg.length()){
        return last_result = Resultcode::ERR_WRITE;
    }
    http_reply = read_http_reply(sck,debug);
    return last_result = Resultcode::OK;
}



//Jenkins Monitoring: Builds

class Monitor_jenkins_builds{
    std::unordered_map<std::string,int> str2idx;
    std::unordered_map<int,std::string> idx2str;
    using queue_t = threadsafe_queue<job_ext_t,std::queue<job_ext_t>>;
    queue_t monitor_queue;
    Job_Control* job_control = nullptr;
public:
    void monitor(job_ext_t job);
    void set_job_control(Job_Control* job_control_) {job_control = job_control_;}

    bool debug = false;
    struct db{
        struct entry{
            struct param{
                int name,value;
            };
            std::uint64_t timestamp;
            std::uint64_t build_number;
            std::string result;
            std::string key;
        };
        std::vector<entry> builds;
        std::vector<db::entry::param> params;
        std::vector<int> entryidx2params;
        std::unordered_map<std::string,int> key2entry;
        std::string compute_key(std::unordered_map<std::string,int>& str2idx,std::unordered_map<int,std::string>& idx2str,std::vector< std::pair<std::string,sm4ceps_plugin_int::Variant> >& ps){
            std::string k = "_";
            if (ps.size()==0) return k;
            std::vector<std::pair<int,int>> v;
            for(auto& e:ps){
                auto it_pn = str2idx.find(e.first);
                auto idx_pn = 0;
                if (it_pn == str2idx.end()){
                    idx_pn = str2idx.size() + 1;
                    str2idx[e.first] = idx_pn;
                    idx2str[idx_pn] = e.first;
                } else idx_pn = it_pn->second;
                std::string value;
                if (e.second.what_== sm4ceps_plugin_int::Variant::String)
                    value = e.second.sv_;
                else if (e.second.what_ == sm4ceps_plugin_int::Variant::Int)
                    value = std::to_string(e.second.iv_);
                else value = std::to_string(e.second.dv_);
                auto it_pv = str2idx.find(value);

                auto idx_pv = 0;
                if (it_pv == str2idx.end()){
                    idx_pv = str2idx.size() + 1;
                    str2idx[value] = idx_pv;
                    idx2str[idx_pv] = value;
                } else idx_pv = it_pv->second;
                v.push_back({idx_pn,idx_pv});
            }
            std::sort(v.begin(),v.end(),[](std::pair<int,int> const & a,std::pair<int,int> const & b){return a.first < b.first; });
            for(auto const & e: v)
                k += std::to_string(e.first)+"_";
            for(auto const & e: v)
                k += "$" + std::to_string(e.second);

            return k;
        }
    };

private:
 struct Rapidjson_handler {
     std::vector<db::entry>& builds;
     std::vector<db::entry::param>& params;
     std::vector<int>& entryidx2params;
     std::unordered_map<std::string,int>& str2idx;
     std::unordered_map<int,std::string>& idx2str;

     Rapidjson_handler(std::vector<db::entry>& b,
                       std::unordered_map<std::string,int>& str2idx_p,
                       std::unordered_map<int,std::string>& idx2str_p,
                       std::vector<db::entry::param>& params_p,
                       std::vector<int>& entryidx2params_p

                       ):builds{b},str2idx{str2idx_p},idx2str{idx2str_p},params{params_p},entryidx2params{entryidx2params_p}{}


            bool builds_key_read = false;
            bool inside_build_list = false;
            int indent = 0;
            int obj_depth = 0;
            db::entry current_entry;

            int param_value_name_ctr=0;
            bool set_param_name = false;
            bool set_param_value = false;
            bool timestamp_key_read = false;
            bool number_key_read = false;
            bool result_key_read = false;
            bool key_read = false;


            std::string last_param_name;
            std::string last_param_value;


            bool Null(){
                if(key_read && result_key_read){
                    current_entry.result = "";
                }
                key_read = false;

                return true;
            }
            //{ std::cout << "Null()" << std::endl; return true; }
            bool Bool(bool b){
                key_read = false;
                return true;
            }
            //{ std::cout << "Bool(" << std::boolalpha << b << ")" << std::endl; return true; }
            bool Int(int i){

                key_read = false;
                return true;
            }
            //{ std::cout << "Int(" << i << ")" << std::endl; return true; }
            bool Uint(unsigned u){
                if(key_read && number_key_read){
                    current_entry.build_number = u;
                }
                key_read = false;
                return true;
            }
            //{ std::cout<< "Uint(" << u << ")" << std::endl; return true; }
            bool Int64(int64_t i){
                key_read = false;
                return true;
            }
            //{ std::cout << "Int64(" << i << ")" << std::endl; return true; }
            bool Uint64(uint64_t u){
                if(key_read && timestamp_key_read){
                    current_entry.timestamp = u;
                }
                key_read = false;
                return true;
            }
            //{ std::cout << "Uint64(" << u << ")" << std::endl; return true; }
            bool Double(double d){
                key_read = false;
                return true;
            }
            //{ std::cout << "Double(" << d << ")" << std::endl; return true; }
            bool RawNumber(const char* str, rapidjson::SizeType length, bool copy){
                if(key_read && timestamp_key_read){
                    std::cerr << str << std::endl;
                }
                key_read = false;
                return true;
            }
            //{
            //    std::cout << "Number(" << str << ", " << length << ", " << std::boolalpha << copy << ")" << std::endl;
            //    return true;
            //}

            bool String(const char* str, rapidjson::SizeType length, bool copy){
                //std::cerr << str << "\n";
                if (set_param_name){
                    ++param_value_name_ctr;
                    last_param_name = str;
                } else if (set_param_value){
                    last_param_value = str;
                    ++param_value_name_ctr;
                }

                if(key_read && result_key_read){
                    current_entry.result = str;
                }

                key_read = false;
                return true;
            }
            //{
            //    std::cout << "String(" << str << ", " << length << ", " << std::boolalpha << copy << ")" << std::endl;
            //    return true;
            // }
            bool StartObject() {
                //std::cerr << "so\n";
                if (inside_build_list) {
                    param_value_name_ctr = 0;
                    set_param_name = set_param_value = false;
                    if(obj_depth == 0){
                        entryidx2params.push_back(params.size());
                        params.push_back({0,0});
                        current_entry = db::entry{};
                    }
                    ++obj_depth;
                }
                key_read = false;
                return true;
            }
           //{ std::cout << "StartObject()" << std::endl; return true; }
            bool Key(const char* str, rapidjson::SizeType length, bool copy) {
                key_read = false;
                if (strcmp(str,"allBuilds") == 0) {
                    builds_key_read = true;
                }
                if (inside_build_list){
                    set_param_name = false;
                    set_param_value = false;
                    if (strcmp(str,"name")==0){
                        set_param_name = key_read= true;
                    } else if (strcmp(str,"value")==0){
                        set_param_value = key_read = true;
                    } else if (strcmp(str,"timestamp")==0){
                        timestamp_key_read = key_read = true;
                    } else if (strcmp(str,"number")==0){
                        number_key_read = key_read = true;
                    } else if (strcmp(str,"result")==0){
                        result_key_read = key_read = true;
                    }
                }
                return true;
            }
            bool EndObject(rapidjson::SizeType memberCount) {
                if (inside_build_list){
                    if (param_value_name_ctr == 2){
                        auto it_name = str2idx.find(last_param_name);
                        auto it_value = str2idx.find(last_param_value);
                        int pidx,vidx;
                        if (it_name != str2idx.end()) pidx = it_name->second; else {
                            pidx = str2idx.size()+1;str2idx[last_param_name] = pidx;
                            idx2str[pidx] = last_param_name;
                        }
                        if (it_value != str2idx.end()) vidx = it_value->second;else {
                            vidx = str2idx.size()+1;str2idx[last_param_value]=vidx;
                            idx2str[vidx] = last_param_value;
                        }
                        params[params.size()-1].name = pidx;
                        params[params.size()-1].value = vidx;
                        params.push_back({0,0});
                    }
                    if (obj_depth == 1){
                        builds.push_back(current_entry);
                    }
                    param_value_name_ctr = 0;
                    --obj_depth;
                }
                return true;
            }
            bool StartArray() {
                if (inside_build_list){
                    ++indent;
                }
                else if (builds_key_read) {
                    builds_key_read = false;
                    inside_build_list = true;
                    indent = 0;
                }
                return true;
            }
            bool EndArray(rapidjson::SizeType elementCount) {
                if (inside_build_list){
                    if(indent==0) inside_build_list = false;
                    else --indent;
                }
                return true;
            }
        };

private:
    std::map<std::string, db*> job2db;
    mutable std::mutex m_;
    struct monitor_info_t{
        std::vector<std::pair<bool,job_ext_t>> jobs;
        db * database = nullptr;
    };
    std::map<std::tuple<std::string,std::string,std::string>,monitor_info_t> monitored_jobs;
    void monitoring_thread();
    std::thread* main_monitoring_thread = nullptr;
    mutable std::mutex monitor_mtx;
public:
    enum build_status {NOT_FOUND,RUNNING,SUCCESS,FAILURE};
    db* build_job_status_db(std::string job_name,std::string hostname,std::string port,std::string authorization, std::string jenkins_crumb,int window = 20000){
        std::stringstream ss;
        ss << "GET /job/" << job_name << "/api/json?&pretty=true&tree=allBuilds[number,timestamp,actions[parameters[name,value]],result]{0,"<<window<<"} HTTP/1.1\r\n";
        ss << "Host: "<<hostname;
        if (port.length()) ss<<":"<<port;
        ss<< "\r\n";
        ss<<"User-Agent: RollAut/0.0.1\r\n";
        ss<<"Accept: */*\r\n";
        ss<<"Authorization: Basic "<< authorization <<"\r\n";
        ss<<"Jenkins-Crumb:"<< jenkins_crumb <<"\r\n";
        ss<< "\r\n";
        http_request_and_reply read_allbuilds{hostname,port,ss.str()};
        {
            using namespace rapidjson;
            db* d = new db;
            Rapidjson_handler handler{d->builds, str2idx, idx2str,d->params,d->entryidx2params};
            Reader reader;
            std::string temp = read_allbuilds.http_reply.content.str();
            //std::cout << temp <<std::endl<<"******************************\n\n\n\n";
            StringStream ss(temp.c_str());
            reader.Parse(ss, handler);
            for(auto i = 0; i != d->builds.size();++i){
                std::vector<int> ps;
                auto params_start = d->entryidx2params[i];
                for(auto j = params_start;d->params[j].name;++j){
                    ps.push_back(d->params[j].name);
                }
                if(ps.size()) std::sort(ps.begin(),ps.end());
                std::string k = "_";
                for(auto l = 0; l != ps.size();++l){
                    k += std::to_string(ps[l])+"_";
                }
                for(auto l = 0; l != ps.size();++l){
                    for(auto ll = 0; ll != ps.size();++ll){
                        if (ps[l] == d->params[params_start+ll].name){
                            k += "$"+std::to_string(d->params[params_start+ll].value);
                            break;
                        }
                    }
                }
                auto it = d->key2entry.find(k);
                if (it == d->key2entry.end()) d->key2entry[k] = i;
                d->builds[i].key = k;
            }
            int ctr = 0;
            if (debug) for(auto& b : d->builds){
                std::cout << "-----------\n" << std::endl;
                std::cout << "build_number:"<< b.build_number << std::endl;
                std::cout << "timestamp:"<< b.timestamp << std::endl;
                std::cout << "key:"<< b.key << std::endl;
                std::cout << "parameters:";
                auto i = d->entryidx2params[ctr];
                for(;d->params[i].name;++i){
                    std::cout << " " << idx2str[d->params[i].name]<<"("<< d->params[i].name <<")"<< "="<< " ("<< d->params[i].value <<")"  <<idx2str[d->params[i].value] << "\n";
                }
                std::cout << "\n";
                ++ctr;
            }
            if (debug)std::cout << d->builds.size()<<std::endl;
            return d;
        }
    }
    std::pair<build_status,db::entry> get_build_info(job_ext_t job,std::string authorization, std::string jenkins_crumb,int window = 1000){
        db* database = nullptr;
        auto it = job2db.find(job.job_name);
        if (it == job2db.end()){
            database = build_job_status_db(job.job_name,job.hostname,job.port,authorization,jenkins_crumb,window = 1000);
        } else database = it->second;
        if (job.key.length() == 0)
            job.key = database->compute_key(str2idx,idx2str,job.params);
        auto it_entry = database->key2entry.find(job.key);
        if(it_entry == database->key2entry.end()) return {NOT_FOUND,{}};
        auto & e = database->builds[it_entry->second];
        if (e.result.length()==0)
          return {RUNNING,e};
        if (e.result == "SUCCESS")
            return {SUCCESS,e};
        return{FAILURE,e};
    }
};

Monitor_jenkins_builds monitor_jenkins_builds;







//
// Thread to trigger jobs via HTTP POST
//

void control_job_thread_fn(int max_tries,
                         std::chrono::milliseconds delta,
                         std::string host,
                         std::string user,
                         std::string credentials,
                         worker_info_t::queue_t* q,
                         Monitor_jenkins_builds* mjb,
                         Jenkinsplugin* jenkinsplugin){

    std::queue<job_ext_t> jq;
    std::queue<job_ext_t> actions;


    auto fetch_new_jobs = [&](){
            std::lock_guard<std::mutex> lk(q->data_mutex());
            for(;!q->data().empty();){
                job_ext_t e = q->data().front();
                e.fetched = std::chrono::system_clock::now();
                e.fetched_hr = std::chrono::high_resolution_clock::now();
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

        for(;jobs_to_process;--jobs_to_process){
            current_job = jq.front();
            if (!current_job.authorization_complete()){
                jq.pop();
                jenkinsplugin->push_to_authenticator_queue(current_job);
                continue;
            }
            jq.pop();

            std::stringstream ss;
            ss << "POST /job/" << current_job.job_name << "/build HTTP/1.1\r\n";
            ss << "Host: "<<current_job.hostname;
            if (current_job.port.length()) ss<<":"<<current_job.port;
            ss<< "\r\n";
            ss<<"User-Agent: RollAut/0.0.1\r\n";
            ss<<"Accept: */*\r\n";
            ss<<"Authorization: Basic "<< current_job.auth.authorization <<"\r\n";
            ss<<"Jenkins-Crumb:"<< current_job.auth.crumb <<"\r\n";

            if (current_job.json_rep_of_params_url_encoded.length()){
                ss<<"Content-Length: "<<current_job.json_rep_of_params_url_encoded.length()<<"\r\n";
                ss<<"Content-Type: application/x-www-form-urlencoded\r\n\r\n";
                ss<<current_job.json_rep_of_params_url_encoded;
            } else ss<< "\r\n";
            current_job.triggered = std::chrono::system_clock::now();
            current_job.triggered_hr = std::chrono::high_resolution_clock::now();

            http_request_and_reply trigger_job_request{current_job.hostname,current_job.port,ss.str()};

            auto& http_reply = trigger_job_request.http_reply;
            if (http_reply.reply_code / 100 == 2){
                if(!current_job.follow_status)
                    plugin_master->queue_event(current_job.ev_done,{sm4ceps_plugin_int::Variant{current_job.job_name}});
                else{
                    mjb->monitor(current_job);
                }
            } else {
                plugin_master->queue_event(current_job.ev_fail,{sm4ceps_plugin_int::Variant{current_job.job_name},
                                                                sm4ceps_plugin_int::Variant{"Triggering job failed ("+http_reply.header+")"}});
            }
            //std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }//Loop through jobs once
        //std::this_thread::sleep_for(std::chrono::milliseconds(1000));
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


//
// cepS plugin part
//

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

    auto trim = [=](std::string & s) {
        if (s.length() == 0) return;
        auto a = 0;
        for(;a < s.length() && s[a] == ' ';++a);
        if (a == s.length()) {s="";return;}
        auto b = s.length()-1;
        for(;b > a && s[b] == ' ' ;--b);
        if (a != 0 || b != s.length()-1) s = s.substr(a,b-a+1);
    };

    std::vector<ceps::ast::Nodebase_ptr> args;
    if (params != nullptr && params->children().size()) flatten_args(params->children()[0], args, ',');

    job_t job;
    job.ev_fail = "event_jenkins_plugin_failed";
    job.ev_done = "event_jenkins_plugin_done";
    job.command = "build";

    for(auto p: args){
        if (p->kind() == Ast_node_kind::binary_operator && op(as_binop_ref(p)) == '='){
            auto & root = as_binop_ref(p);
            auto  l_ = root.left(); auto r_ = root.right();
            if (l_->kind() == Ast_node_kind::symbol){
                auto & l = as_symbol_ref(l_);
                if (kind(l) != "Formal_parameter_name" && kind(l) != "Parameter") continue;
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
                    else if (lhs_name == "on_job_queued") job.ev_job_queued = rhs_name;
                } else {
                    if (lhs_name == "hostname" && r_->kind() == Ast_node_kind::string_literal)
                        job.hostname = value(as_string_ref(r_));
                    else if (lhs_name == "port" && r_->kind() == Ast_node_kind::string_literal)
                        job.port = value(as_string_ref(r_));
                    else if (lhs_name == "url" && r_->kind() == Ast_node_kind::string_literal){
                        std::string url = value(as_string_ref(r_));
                        {
                           auto p = url.find("http://");
                           if (p != std::string::npos)
                               url = url.substr(p+7);
                        }
                        auto host = url;
                        std::string port = "80";
                        {
                            auto p = host.find_first_of(":");
                            if (p != std::string::npos){
                                auto start_port = p+1;
                                for(;start_port < host.length() && host[start_port] == ' ';++start_port);
                                auto end_port = start_port;
                                for(;end_port < host.length() && std::isdigit(host[end_port]);++end_port);
                                --end_port;
                                if(start_port <= end_port && host.length() > end_port)
                                    port = host.substr(start_port,end_port-start_port+1);
                                host = host.substr(0,p);
                            }
                        }
                        trim(host);
                        if(host.length()) job.hostname = host;
                        if(port.length()) job.port = port;
                    }
                    else if (lhs_name == "authorization" && r_->kind() == Ast_node_kind::string_literal){
                        job.authorization = value(as_string_ref(r_));
                    }
                    else if (lhs_name == "option" && r_->kind() == Ast_node_kind::string_literal){
                        if ("follow" == value(as_string_ref(r_))){
                            job.follow_status = true;
                        }
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
    jenkinsplugin.issue_job(job);
    return nullptr;
}

extern "C" void init_plugin(IUserdefined_function_registry* smc)
{
  monitor_jenkins_builds.set_job_control(&jenkinsplugin);
  (plugin_master = smc->get_plugin_interface())->reg_ceps_plugin("jenkins",jenkins_plugin);
}

//
//
/////////////////////////////////////// Monitor_jenkins

void Monitor_jenkins_builds::monitor(job_ext_t job){
    static auto a = [&](){main_monitoring_thread = new std::thread{&Monitor_jenkins_builds::monitoring_thread,this}; return true;}();
    monitor_queue.push(job);
}

void Monitor_jenkins_builds::monitoring_thread(){
    std::queue<job_ext_t> jq;
    std::queue<job_ext_t> actions;

    auto fetch_new_jobs = [&](){
            std::lock_guard<std::mutex> lk(monitor_queue.data_mutex());
            for(;!monitor_queue.data().empty();){
                job_ext_t e = monitor_queue.data().front();monitor_queue.data().pop();
                if (e.action.length()==0) jq.push(e);
                else actions.push(e);
            }
    };

    for(;;){
        fetch_new_jobs();
        if(jq.empty() && monitored_jobs.empty()){
            monitor_queue.wait_for_data();
            continue;
        }
        job_ext_t current_job;
        auto jobs_to_process = jq.size();
        for(;jobs_to_process;--jobs_to_process){
            current_job = jq.front();jq.pop();
            auto & info = monitored_jobs[{current_job.hostname,current_job.port,current_job.job_name}];
            bool empty_slot_found = false;
            for(auto& e: info.jobs){
                if (e.first)continue;
                empty_slot_found = true;
                e.second = current_job;
                e.first = true;
                break;
            }
            if (!empty_slot_found) info.jobs.push_back({true,current_job});
        }
        for(auto & monitored_segment : monitored_jobs ){
            bool no_active_jobs = true;std::string authorization, jenkins_crumb;
            for(auto & j : monitored_segment.second.jobs)
                if (j.first) {no_active_jobs = false;authorization = j.second.auth.authorization;jenkins_crumb = j.second.auth.crumb;break;}
            if(no_active_jobs) continue;

            auto ttt = 0;
            for(auto & j : monitored_segment.second.jobs)
                if (j.first) ++ttt;

            //std::cerr << ttt <<" active jobs\n";
            //std::cerr << monitored_segment.second.jobs.size() << " entries total\n";

            bool new_data_available = false;
            auto & database = monitored_segment.second.database;
            if (database != nullptr) {delete database;database = nullptr;}

            if (database == nullptr){
                new_data_available = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                database =  build_job_status_db(std::get<2>(monitored_segment.first),
                                                std::get<0>(monitored_segment.first),
                                                std::get<1>(monitored_segment.first),
                                                authorization, jenkins_crumb);
            }

            if (!new_data_available) continue;
            for(auto & job : monitored_segment.second.jobs){
                if (!job.first) continue;
                if (job.second.key.length() == 0){
                   job.second.key = database->compute_key(str2idx,idx2str,job.second.params);
                }
                ssize_t matching_build_idx = -1;
                if (job.second.associated_buildnumber < 0){
                   long long build_number = -1;
                   long long min_delta = 1;
                   for(ssize_t i = 0; i < database->builds.size(); ++i){
                      auto & e = database->builds[i];
                      if (e.key != job.second.key) continue;
                      long long t = std::chrono::system_clock::to_time_t(job.second.triggered);
                      long long delta = t - e.timestamp/1000LL;
                      if (delta > 0) continue;
                      if ( delta < min_delta){
                         build_number = e.build_number;
                         min_delta = delta;
                         matching_build_idx = i;
                      }
                   }
                   if (build_number >= 0){
                      job.second.associated_buildnumber = build_number;
                      job.second.queued_hr = std::chrono::high_resolution_clock::now();
                      auto d = job.second.queued_hr - job.second.triggered_hr;
                      auto t_ms = std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
                      if (job.second.ev_job_queued.length() && job_control) job_control->ev(job.second.ev_job_queued,{sm4ceps_plugin_int::Variant{job.second.job_name},
                                                                      sm4ceps_plugin_int::Variant{std::to_string(build_number)},
                                                                                                                      sm4ceps_plugin_int::Variant{(((double)t_ms)/1000.000)} });
                   }
                } else {
                    for(ssize_t i = 0; i < database->builds.size(); ++i){
                        if (database->builds[i].build_number != job.second.associated_buildnumber) continue;
                        matching_build_idx = i; break;
                    }
                }
                if (matching_build_idx < 0) continue;
                if (database->builds[matching_build_idx].result.length()==0) continue;
                //std::cerr << job.second.params[0].second.iv_ << std::endl;
                job.first = false;
                if (database->builds[matching_build_idx].result == "SUCCESS")
                {
                    if (job_control && job.second.ev_done.length()) job_control->ev(job.second.ev_done,{sm4ceps_plugin_int::Variant{job.second.job_name},
                                                                sm4ceps_plugin_int::Variant{"SUCCESS (Jenkinsbuild number: "+std::to_string(job.second.associated_buildnumber)+")"}});
                } else {
                    if(job_control && job.second.ev_fail.length()) job_control->ev(job.second.ev_fail,{sm4ceps_plugin_int::Variant{job.second.job_name},
                                                                sm4ceps_plugin_int::Variant{database->builds[matching_build_idx].result+
                                                                " (Jenkinsbuild number: "+std::to_string(job.second.associated_buildnumber)+")"}});
                }
            }//for
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
}

//Jenkins Plugin Implementation



void Jenkinsplugin::issue_job(job_t job){
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
        workers[wk] = worker_info_t{new std::thread{control_job_thread_fn,
                                                    10,
                                                    std::chrono::milliseconds{10},
                                                    job.hostname,"","",q,&monitor_jenkins_builds,this}, q};
        it = workers.find(wk);
    }
    job.worker_id = wk;

    it->second.job_queue->push(job);
}

void Jenkinsplugin::ev(std::string ev_name,std::initializer_list<sm4ceps_plugin_int::Variant> vl){
    plugin_master->queue_event(ev_name,vl);
}

bool Jenkinsplugin::push_to_worker_queue(job_ext_t const & job){
    auto const & wk = job.worker_id;
    auto it = workers.find(wk);
    if (it == workers.end()) return false;
    it->second.job_queue->push(job);
    return true;
}


bool Jenkinsplugin::push_to_authenticator_queue(job_ext_t const & job) {
    authenticator_info_t::queue_t* q = nullptr;
    auto it = authenticators.find({job.hostname,job.port});
    if (it == authenticators.end()){
        q = new authenticator_info_t::queue_t{};
        authenticator_info_t a{new std::thread{&Jenkinsplugin::authenticator_thread,this,q},q };
        authenticators[{job.hostname,job.port}] = a;
    } else q = it->second.job_queue;
    q->push(job);
    return true;
}

void Jenkinsplugin::authenticator_thread(authenticator_info_t::queue_t* q){
    std::queue<job_ext_t> jq;
    std::queue<job_ext_t> actions;

    auto fetch_new_jobs = [&](){
            std::lock_guard<std::mutex> lk(q->data_mutex());
            for(;!q->data().empty();){
                job_ext_t e = q->data().front();q->data().pop();
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

        for(;jobs_to_process;--jobs_to_process){
            current_job = jq.front();jq.pop();
            std::string jenkins_crumb;
            {
                std::lock_guard<std::mutex> lk(host_and_port2crumbs_mtx);
                jenkins_crumb = host_and_port2crumbs[std::make_pair(current_job.hostname,current_job.port)];
            }
            std::string authorization = encode_base64(current_job.authorization.data(),current_job.authorization.length());//"dG9tYXM6bEFLdGF0Mzcs";

            if (jenkins_crumb.length() == 0){
                std::stringstream ss;
                ss << "GET /crumbIssuer/api/xml?xpath=concat(//crumbRequestField,%22:%22,//crumb) HTTP/1.1\r\n";
                ss << "Host: "<<current_job.hostname;
                if (current_job.port.length()) ss<<":"<<current_job.port;
                ss<< "\r\n";
                ss<<"User-Agent: RollAut/0.0.1\r\n";
                ss<<"Accept: */*\r\n";
                ss<<"Authorization: Basic "<< authorization <<"\r\n";
                ss<< "\r\n";
                http_request_and_reply read_crumb{current_job.hostname,current_job.port,ss.str()};
                if (read_crumb.last_result != http_request_and_reply::Resultcode::OK){

                    plugin_master->queue_event(current_job.ev_fail,{sm4ceps_plugin_int::Variant{current_job.job_name},
                                                                    sm4ceps_plugin_int::Variant{"Fetching Jenkins Crumb Failed ("+http_request_and_reply::result_str(read_crumb.last_result)+")"}});
                    continue;
                }

                if ( read_crumb.http_reply.reply_code / 100  == 2 && read_crumb.http_reply.content_length){
                    auto s = read_crumb.http_reply.content.str();
                    auto n = s.length();
                    auto sp = read_crumb.http_reply.content.str().find_last_of(":");
                    if (sp == std::string::npos) continue;
                    auto t = read_crumb.http_reply.content.str().substr(sp+1,n-sp-1);
                    {
                        std::lock_guard<std::mutex> lk(host_and_port2crumbs_mtx);
                        jenkins_crumb=host_and_port2crumbs[std::make_pair(current_job.hostname,current_job.port)]=t;
                    }
                } else {
                    plugin_master->queue_event(current_job.ev_fail,{sm4ceps_plugin_int::Variant{current_job.job_name},
                                                                    sm4ceps_plugin_int::Variant{"Fetching Jenkins Crumb Failed ("+read_crumb.http_reply.header+")"}});
                }
            }
            if (jenkins_crumb.length() == 0) continue;
            current_job.auth.crumb = jenkins_crumb;
            current_job.auth.authorization = authorization;
            push_to_worker_queue(current_job);
        }//Loop through jobs once
    }
}


