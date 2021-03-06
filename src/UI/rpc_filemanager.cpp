#include "../../../AppWebServer/jrpc.hpp"
#include "../Dispatcher.hpp"
#include <boost/filesystem.hpp>
#include "../tools.hpp"

using namespace std;
namespace fs = boost::filesystem;
typedef const AWS::JSON MP;

string hash2str(const Hash& h)
{
  if (h.size() != 16)
	throw HashInvalid();
  char buf[33];
  for (int i=0; i<16; i++) {
	  snprintf(buf+i*2, 3, "%02x", (unsigned char)h[i]);
  }
  return string(buf, 33);
}
Hash str2hash(const string& s)
{
  try {
	  Hash hash(16, ' ');
	  for (int i=0; i<32; i+=2) {
		  hash[i/2] = stoi(s.substr(i, 2), 0, 16);
	  }
	  return hash;
  } catch (...) {
	  throw HashInvalid(s);
  }
}

MP info2json(const FInfo& info)
{
  AWS::JSON node;
  node["hash"] = hash2str(info.hash);
  node["name"] = to_utf8(fs::path(to_ucs2(info.path)).filename().wstring());
  assert(info.chunknum > 0);
  node["size"] = (info.chunknum-1) * CHUNK_SIZE + info.lastchunksize;
  node["status"] = info.status;

  //当前本版不支持文件目录因此全是文件类型(0)
  node["type"] = 0;
  return node;
}


MP msg2json(const NewMsg& msg)
{
  AWS::JSON result; result["newfile"]; result["pfile"]; result["progress"];
  for (auto& file: msg.new_files) {
	  result["newfile"].append(info2json(file));
  }
  result["payload"] = msg.payload.global;
  for (auto& payload: msg.payload.files) {
	  static AWS::JSON node;
	  node["hash"] = hash2str(payload.first);
	  node["value"] = payload.second;
	  result["pfile"].append(node);
  }
  for (auto& p : msg.progress) {
	  static AWS::JSON node;
	  node["hash"] = hash2str(p.first);
	  node["value"] = p.second;
	  result["progress"].append(node);
  }
  return result;
}


AWS::Service& rpc_dispatcher(Dispatcher& dispatcher)
{
  static AWS::Service the_dispatcher;
  the_dispatcher.name = "filemanager";
  the_dispatcher.methods = {
		{
		  "file_list", [&](MP& j){
			  AWS::JSON result;
			  for (auto i : dispatcher.current_list()) {
				  result.append(info2json(i));
			  }
			  return result;
		  }
		},
		{
		  "add_file", [&](MP& j){
			  try {
				  dispatcher.add_local_file(j["path"].asString());
				  return AWS::JSON("添加文件成功");
			  } catch (InfoExists&) {
				  return AWS::JSON("已经存在此文件"); 
			  } catch (std::exception& e){
				  throw;
				  return AWS::JSON(string("未知错误")+e.what()); 
			  }
		  }
		},
		{
		  "del_file", [&](MP& j){
			  try {
				  dispatcher.remove(str2hash(j["hash"].asString()));
				  return AWS::JSON("成功删除");
			  } catch (InfoNotFound&) {
				  return AWS::JSON("所删除文件不存在");
			  } catch (HashInvalid& e) {
				  return AWS::JSON(string("所删除文件哈希无效:") + e.what());
			  }
		  }
		},
		{
		  "download", [&](MP& j){
			  try {
				  dispatcher.start_download(str2hash(j["hash"].asString()));
				  return AWS::JSON("下载请求提交成功");
			  } catch (InfoNotFound&) {
				  return AWS::JSON("文件不存在");
			  } catch (HashInvalid& e) {
				  return AWS::JSON(string("文件哈希无效:") + e.what());
			  }
		  }
		},
		{
		  "refresh", [&](MP& j){
			  NewMsg msg = dispatcher.refresh();
			  return msg2json(msg);
		  }
		},
		{
		  "chunk_info", [&](MP& j){
			  auto b = dispatcher.chunk_info(str2hash(j["hash"].asString()));
			  return b;
		  }
		}
  };
  return the_dispatcher;
}


