#include <sentencepiece_processor.h>
#include <ctranslate2/translator_pool.h>
#include <ctranslate2/models/sequence_to_sequence.h>
#include <fstream>
#include <regex>

static std::vector<std::string> get_vocabulary_tokens(const ctranslate2::Vocabulary& vocabulary) {
  std::vector<std::string> tokens;
  const size_t size = vocabulary.size();
  tokens.reserve(size);
  for (size_t i = 0; i < size; ++i)
    tokens.emplace_back(vocabulary.to_token(i));
  return tokens;
}

int main(int, char* argv[]) {
  const std::string in_file = argv[1];
  const std::string out_file = argv[2];
  const int num_cores = std::stoi(std::string(argv[3]));

  const std::string model_path = "/model";
  const std::string sp_model_path = model_path + "/sp.model";

  const ctranslate2::Device device = ctranslate2::str_to_device("auto");
  ctranslate2::ComputeType compute_type = ctranslate2::ComputeType::INT8;
  size_t num_threads_per_replica = 1;
  size_t num_replicas = 0;
  size_t max_batch_size = 0;
  if (device == ctranslate2::Device::CUDA) {
    num_replicas = 1;
    max_batch_size = 6000;
    compute_type = ctranslate2::ComputeType::FLOAT16;
  } else if (num_cores == 1) {
    num_replicas = 1;
    max_batch_size = 512;
  } else {
    num_replicas = num_cores / 2;
    max_batch_size = 256;
  }

  const auto model = ctranslate2::models::Model::load(model_path, device, 0, compute_type);
  ctranslate2::TranslatorPool pool(num_replicas, num_threads_per_replica, model);

  sentencepiece::SentencePieceProcessor sp_processor;
  auto status = sp_processor.Load(sp_model_path);
  if (!status.ok())
    throw std::invalid_argument("Unable to open SentencePiece model " + sp_model_path);
  const auto* seq2seq_model = dynamic_cast<const ctranslate2::models::SequenceToSequenceModel*>(model.get());
  status = sp_processor.SetVocabulary(get_vocabulary_tokens(seq2seq_model->get_source_vocabulary()));
  if (!status.ok())
    throw std::runtime_error("Failed to set the SentencePiece vocabulary");

  auto reader = [&sp_processor](std::istream& in, std::vector<std::string>& tokens) {
                  std::string line;
                  if (!std::getline(in, line))
                    return false;
                  sp_processor.Encode(line, &tokens);
                  return true;
                };

  auto writer = [&sp_processor](std::ostream& out,
                                const ctranslate2::TranslationResult& result) {
                  std::string text;
                  sp_processor.Decode(result.output(), &text);
                  out << std::regex_replace(text, std::regex("<unk>"), "UNK") << '\n';
                };

  ctranslate2::TranslationOptions options;
  options.beam_size = 1;
  options.max_decoding_length = 150;
  options.max_batch_size = max_batch_size;
  options.batch_type = ctranslate2::BatchType::Tokens;
  options.use_vmap = true;
  options.return_scores = false;

  std::ifstream in(in_file);
  std::ofstream out(out_file);
  pool.consume_stream(in, out, max_batch_size * 8, options, reader, writer);
  out.flush();
  return 0;
}
