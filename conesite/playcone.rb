require 'sinatra/base'
require 'open3'
require 'json'

class PlayCone < Sinatra::Base
  get "/" do
	send_file File.expand_path('index.html', settings.public_folder)
  end

  post "/play/evaluate.json" do
	# extract and save the code from JSON payload
	request.body.rewind
	body = JSON.parse request.body.read
	File.open("work/test.cone", 'w') { |file| file.write(body['code']) }
	
	# Run compile and generate hash object with requested file or error message
	prog_out = ""
	rc, stdout, conec_err = docmd("conec -owork/ work/test.cone")
	if rc == 0
		# linkedit with the cone standard library. Path is needed for success
		ENV['PATH'] = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
		rc, stdout, stderr = docmd("gcc -no-pie -o work/test work/test.o /cone/conec/libconestd.a")
		if rc == 0
			norc, prog_out, stderr = docmd("timeout 15s work/test")
		else
			conec_err = stderr
		end
	end

	if rc == 0
		output = {'conec' => conec_err, 'program' => prog_out}
	else
		output = {'conec' => conec_err}
	end
	response = JSON.generate(output)
	content_type :json
	response
  end
  
  post "/play/compile.json" do
	# extract and save the code from JSON payload
	request.body.rewind
	body = JSON.parse request.body.read
	File.open("work/test.cone", 'w') { |file| file.write(body['code']) }
	
	# Run compile and generate hash object with requested file or error message
	rc, stdout, error = docmd("conec --asm --llvmir -owork/ work/test.cone")
	if rc == 0
		# retrieve results on success
		resultfile = body['emit'] == 'asm' ? "work/test.s" : "work/test.ir"
		output = {'result' => File.read(resultfile)}
	else
		output = {'error' => error}
	end
	
	# Return JSON-encoded requested file or error message 
	response = JSON.generate(output)
	content_type :json
	response
  end
end

# Run a command and capture output
def docmd(cmd)
	stdout, stderr, status = Open3.capture3(cmd)
	rc = status.exitstatus
	return rc, stdout, stderr
end
